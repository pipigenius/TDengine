/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "os.h"
#include "taosmsg.h"
#include "tlog.h"
#include "trpc.h"
#include "tstatus.h"
#include "tsched.h"
#include "dnodeSystem.h"
#include "mnode.h"
#include "mgmtAcct.h"
#include "mgmtBalance.h"
#include "mgmtChildTable.h"
#include "mgmtConn.h"
#include "mgmtDb.h"
#include "mgmtDnode.h"
#include "mgmtGrant.h"
#include "mgmtMnode.h"
#include "mgmtNormalTable.h"
#include "mgmtProfile.h"
#include "mgmtShell.h"
#include "mgmtStreamTable.h"
#include "mgmtSuperTable.h"
#include "mgmtTable.h"
#include "mgmtUser.h"
#include "mgmtVgroup.h"


#define MAX_LEN_OF_METER_META (sizeof(SMultiTableMeta) + sizeof(SSchema) * TSDB_MAX_COLUMNS + sizeof(SSchema) * TSDB_MAX_TAGS + TSDB_MAX_TAGS_LEN)

typedef int32_t (*GetMateFp)(STableMeta *pMeta, SShowObj *pShow, void *pConn);
typedef int32_t (*RetrieveMetaFp)(SShowObj *pShow, char *data, int32_t rows, void *pConn);
static GetMateFp* mgmtGetMetaFp;
static RetrieveMetaFp* mgmtRetrieveFp;
static void mgmtInitShowMsgFp();

void *tsShellConnServer = NULL;

static void mgmtInitProcessShellMsg();
static void mgmtProcessMsgFromShell(char type, void *pCont, int contLen, void *ahandle, int32_t code);
static int32_t (*mgmtProcessShellMsg[TSDB_MSG_TYPE_MAX])(void *pCont, int32_t contLen, void *ahandle);
static int32_t mgmtProcessUnSupportMsg(void *pCont, int32_t contLen, void *ahandle);
static int32_t mgmtRetriveUserAuthInfo(char *user, char *spi, char *encrypt, char *secret, char *ckey);

void mgmtProcessTranRequest(SSchedMsg *sched) {
  int8_t  msgType = *(int8_t *) (sched->msg);
  int32_t contLen = *(int32_t *) (sched->msg + sizeof(int8_t));
  int8_t  *pCont  = sched->msg + sizeof(int32_t) + sizeof(int8_t);
  void    *pConn  = sched->thandle;

  (*mgmtProcessShellMsg[msgType])(pCont, contLen, pConn);
  if (sched->msg) {
    free(sched->msg);
  }
}

void mgmtAddToTranRequest(int8_t type, void *pCont, int contLen, void *ahandle) {
  SSchedMsg schedMsg;
  schedMsg.msg     = malloc(contLen + sizeof(int32_t) + sizeof(int8_t));
  schedMsg.fp      = mgmtProcessTranRequest;
  schedMsg.tfp     = NULL;
  schedMsg.thandle = ahandle;
  *(int8_t *) (schedMsg.msg) = type;
  *(int32_t *) (schedMsg.msg + sizeof(int8_t)) = contLen;
  memcpy(schedMsg.msg + sizeof(int32_t) + sizeof(int8_t), pCont, contLen);

  taosScheduleTask(tsMgmtTranQhandle, &schedMsg);
}

int32_t mgmtInitShell() {
  SRpcInit rpcInit;
  mgmtInitProcessShellMsg();
  mgmtInitShowMsgFp();

  int32_t numOfThreads = tsNumOfCores * tsNumOfThreadsPerCore / 4.0;
  if (numOfThreads < 1) {
    numOfThreads = 1;
  }

  memset(&rpcInit, 0, sizeof(rpcInit));
  rpcInit.localIp      = tsAnyIp ? "0.0.0.0" : tsPrivateIp;;
  rpcInit.localPort    = tsMgmtShellPort;
  rpcInit.label        = "MND-shell";
  rpcInit.numOfThreads = numOfThreads;
  rpcInit.cfp          = mgmtProcessMsgFromShell;
  rpcInit.sessions     = tsMaxShellConns;
  rpcInit.connType     = TAOS_CONN_SERVER;
  rpcInit.idleTime     = tsShellActivityTimer * 2000;
  rpcInit.afp          = mgmtRetriveUserAuthInfo;

  tsShellConnServer = rpcOpen(&rpcInit);
  if (tsShellConnServer == NULL) {
    mError("failed to init tcp connection to shell");
    return -1;
  }

  return 0;
}

void mgmtCleanUpShell() {
  if (tsShellConnServer) {
    rpcClose(tsShellConnServer);
    tsShellConnServer = NULL;
  }
}

int32_t mgmtProcessTableMetaMsg(void *pCont, int32_t contLen, void *ahandle) {
  SRpcConnInfo connInfo;
  rpcGetConnInfo(ahandle, &connInfo);

  bool usePublicIp = (connInfo.serverIp == tsPublicIpInt);
  SUserObj *pUser = mgmtGetUser(connInfo.user);
  if (pUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  STableInfoMsg *pInfo = pCont;
  pInfo->createFlag = htons(pInfo->createFlag);

  SDbObj* pDb = mgmtGetDbByTableId(pInfo->tableId);
  if (pDb == NULL || pDb->dropStatus != TSDB_DB_STATUS_READY) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_DB, NULL, 0);
    return TSDB_CODE_INVALID_DB;
  }

  STableInfo *pTable = mgmtGetTable(pInfo->tableId);

  // on demand create table from super table if meter does not exists
  if (pTable == NULL && pInfo->createFlag == 1) {
    // write operation needs to redirect to master mnode
    if (mgmtCheckRedirectMsg(ahandle) != 0) {
      return TSDB_CODE_REDIRECT;
    }

    SCreateTableMsg *pCreateMsg = calloc(1, sizeof(SCreateTableMsg) + sizeof(STagData));
    if (pCreateMsg == NULL) {
      rpcSendResponse(ahandle, TSDB_CODE_SERV_OUT_OF_MEMORY, NULL, 0);
      return TSDB_CODE_SERV_OUT_OF_MEMORY;
    }

    memcpy(pCreateMsg->schema, pInfo->tags, sizeof(STagData));
    strcpy(pCreateMsg->tableId, pInfo->tableId);

    int32_t code = mgmtCreateTable(pDb, pCreateMsg);

    char stableName[TSDB_TABLE_ID_LEN] = {0};
    strncpy(stableName, pInfo->tags, TSDB_TABLE_ID_LEN);
    mTrace("table:%s is auto created by %s from %s, code:%d", pCreateMsg->tableId, pUser->user, stableName, code);

    tfree(pCreateMsg);

    if (code != TSDB_CODE_SUCCESS) {
      rpcSendResponse(ahandle, code, NULL, 0);
      return code;
    }

    pTable = mgmtGetTable(pInfo->tableId);
  }

  if (pTable == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_TABLE, NULL, 0);
    return TSDB_CODE_INVALID_TABLE;
  }

  STableMeta *pMeta = rpcMallocCont(sizeof(STableMeta) + sizeof(SSchema) * TSDB_MAX_COLUMNS);
  int32_t code = mgmtGetTableMeta(pDb, pTable, pMeta, usePublicIp);

  if (code == TSDB_CODE_SUCCESS) {
    rpcFreeCont(pMeta);
    rpcSendResponse(ahandle, TSDB_CODE_SUCCESS, NULL, 0);
  } else {
    pMeta->contLen = htons(pMeta->contLen);
    rpcSendResponse(ahandle, TSDB_CODE_SUCCESS, pMeta, pMeta->contLen);
  }

  return TSDB_CODE_SUCCESS;
}

int32_t mgmtProcessMultiTableMetaMsg(void *pCont, int32_t contLen, void *ahandle) {
  SRpcConnInfo connInfo;
  rpcGetConnInfo(ahandle, &connInfo);

  bool usePublicIp = (connInfo.serverIp == tsPublicIpInt);
  SUserObj *pUser = mgmtGetUser(connInfo.user);
  if (pUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  SMultiTableInfoMsg *pInfo = pCont;
  pInfo->numOfTables = htonl(pInfo->numOfTables);

  int32_t totalMallocLen = 4*1024*1024; // first malloc 4 MB, subsequent reallocation as twice
  SMultiTableMeta *pMultiMeta = rpcMallocCont(totalMallocLen);
  if (pMultiMeta == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_SERV_OUT_OF_MEMORY, NULL, 0);
    return TSDB_CODE_SERV_OUT_OF_MEMORY;
  }

  pMultiMeta->contLen = sizeof(SMultiTableMeta);
  pMultiMeta->numOfTables = 0;

  for (int t = 0; t < pInfo->numOfTables; ++t) {
    char *tableId = (char*)(pInfo->tableIds + t * TSDB_TABLE_ID_LEN);
    STableInfo *pTable = mgmtGetTable(tableId);
    if (pTable == NULL) continue;

    SDbObj *pDb = mgmtGetDbByTableId(tableId);
    if (pDb == NULL) continue;

    int availLen = totalMallocLen - pMultiMeta->contLen;
    if (availLen <= sizeof(STableMeta) + sizeof(SSchema) * TSDB_MAX_COLUMNS) {
      //TODO realloc
      //totalMallocLen *= 2;
      //pMultiMeta = rpcReMalloc(pMultiMeta, totalMallocLen);
      //if (pMultiMeta == NULL) {
      ///  rpcSendResponse(ahandle, TSDB_CODE_SERV_OUT_OF_MEMORY, NULL, 0);
      //  return TSDB_CODE_SERV_OUT_OF_MEMORY;
      //} else {
      //  t--;
      //  continue;
      //}
    }

    STableMeta *pMeta = (STableMeta *)(pMultiMeta->metas + pMultiMeta->contLen);
    int32_t code = mgmtGetTableMeta(pDb, pTable, pMeta, usePublicIp);
    if (code == TSDB_CODE_SUCCESS) {
      pMultiMeta->numOfTables ++;
      pMultiMeta->contLen += pMeta->contLen;
    }
  }

  rpcSendResponse(ahandle, TSDB_CODE_SUCCESS, pMultiMeta, pMultiMeta->contLen);
  return TSDB_CODE_SUCCESS;
}

int32_t mgmtProcessSuperTableMetaMsg(void *pCont, int32_t contLen, void *ahandle) {
  SRpcConnInfo connInfo;
  rpcGetConnInfo(ahandle, &connInfo);

  bool usePublicIp = (connInfo.serverIp == tsPublicIpInt);

  SSuperTableInfoMsg *pInfo = pCont;
  STableInfo *pTable = mgmtGetSuperTable(pInfo->tableId);
  if (pTable == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_TABLE, NULL, 0);
    return TSDB_CODE_INVALID_TABLE;
  }

  SSuperTableInfoRsp *pRsp = mgmtGetSuperTableVgroup((SSuperTableObj *) pTable);
  if (pRsp != NULL) {
    int32_t msgLen = sizeof(SSuperTableObj) + htonl(pRsp->numOfDnodes) * sizeof(int32_t);
    rpcSendResponse(ahandle, TSDB_CODE_SUCCESS, pRsp, msgLen);
    return TSDB_CODE_SUCCESS;
  } else {
    rpcSendResponse(ahandle, TSDB_CODE_SUCCESS, NULL, 0);
    return TSDB_CODE_SUCCESS;
  }
}

int32_t mgmtProcessCreateDbMsg(void *pCont, int32_t contLen, void *ahandle) {
  if (mgmtCheckRedirectMsg(ahandle) != 0) {
    return TSDB_CODE_REDIRECT;
  }

  SUserObj *pUser = mgmtGetUserFromConn(ahandle);
  if (pUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  SCreateDbMsg *pCreate = (SCreateDbMsg *) pCont;

  pCreate->maxSessions     = htonl(pCreate->maxSessions);
  pCreate->cacheBlockSize  = htonl(pCreate->cacheBlockSize);
  pCreate->daysPerFile     = htonl(pCreate->daysPerFile);
  pCreate->daysToKeep      = htonl(pCreate->daysToKeep);
  pCreate->daysToKeep1     = htonl(pCreate->daysToKeep1);
  pCreate->daysToKeep2     = htonl(pCreate->daysToKeep2);
  pCreate->commitTime      = htonl(pCreate->commitTime);
  pCreate->blocksPerTable  = htons(pCreate->blocksPerTable);
  pCreate->rowsInFileBlock = htonl(pCreate->rowsInFileBlock);
  // pCreate->cacheNumOfBlocks = htonl(pCreate->cacheNumOfBlocks);

  int32_t code;
  if (mgmtCheckExpired()) {
    code = TSDB_CODE_GRANT_EXPIRED;
  } else if (!pUser->writeAuth) {
    code = TSDB_CODE_NO_RIGHTS;
  } else {
    code = mgmtCreateDb(pUser->pAcct, pCreate);
    if (code == TSDB_CODE_SUCCESS) {
      mLPrint("DB:%s is created by %s", pCreate->db, pUser->user);
    }
  }

  rpcSendResponse(ahandle, code, NULL, 0);
  return code;
}

int32_t mgmtProcessAlterDbMsg(void *pCont, int32_t contLen, void *ahandle) {
  if (mgmtCheckRedirectMsg(ahandle) != 0) {
    return TSDB_CODE_REDIRECT;
  }

  SUserObj *pUser = mgmtGetUserFromConn(ahandle);
  if (pUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  SAlterDbMsg *pAlter = (SAlterDbMsg *) pCont;
  pAlter->daysPerFile = htonl(pAlter->daysPerFile);
  pAlter->daysToKeep  = htonl(pAlter->daysToKeep);
  pAlter->maxSessions = htonl(pAlter->maxSessions) + 1;

  int32_t code;
  if (!pUser->writeAuth) {
    code = TSDB_CODE_NO_RIGHTS;
  } else {
    code = mgmtAlterDb(pUser->pAcct, pAlter);
    if (code == TSDB_CODE_SUCCESS) {
      mLPrint("DB:%s is altered by %s", pAlter->db, pUser->user);
    }
  }

  rpcSendResponse(ahandle, code, NULL, 0);
  return code;
}

int32_t mgmtProcessKillQueryMsg(void *pCont, int32_t contLen, void *ahandle) {
  if (mgmtCheckRedirectMsg(ahandle) != 0) {
    return TSDB_CODE_REDIRECT;
  }

  SUserObj *pUser = mgmtGetUserFromConn(ahandle);
  if (pUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  SKillQueryMsg *pKill = (SKillQueryMsg *) pCont;
  int32_t code;

  if (!pUser->writeAuth) {
    code = TSDB_CODE_NO_RIGHTS;
  } else {
    code = mgmtKillQuery(pKill->queryId, ahandle);
  }

  rpcSendResponse(ahandle, code, NULL, 0);
  return code;
}

int32_t mgmtProcessKillStreamMsg(void *pCont, int32_t contLen, void *ahandle) {
  if (mgmtCheckRedirectMsg(ahandle) != 0) {
    return TSDB_CODE_REDIRECT;
  }

  SUserObj *pUser = mgmtGetUserFromConn(ahandle);
  if (pUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  SKillStreamMsg *pKill = (SKillStreamMsg *) pCont;
  int32_t code;

  if (!pUser->writeAuth) {
    code = TSDB_CODE_NO_RIGHTS;
  } else {
    code = mgmtKillStream(pKill->queryId, ahandle);
  }

  rpcSendResponse(ahandle, code, NULL, 0);
  return code;
}

int32_t mgmtProcessKillConnectionMsg(void *pCont, int32_t contLen, void *ahandle) {
  if (mgmtCheckRedirectMsg(ahandle) != 0) {
    return TSDB_CODE_REDIRECT;
  }

  SUserObj *pUser = mgmtGetUserFromConn(ahandle);
  if (pUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  SKillConnectionMsg *pKill = (SKillConnectionMsg *) pCont;
  int32_t code;

  if (!pUser->writeAuth) {
    code = TSDB_CODE_NO_RIGHTS;
  } else {
    code = mgmtKillConnection(pKill->queryId, ahandle);
  }

  rpcSendResponse(ahandle, code, NULL, 0);
  return code;
}

int32_t mgmtProcessCreateUserMsg(void *pCont, int32_t contLen, void *ahandle) {
  if (mgmtCheckRedirectMsg(ahandle) != 0) {
    return TSDB_CODE_REDIRECT;
  }

  SUserObj *pUser = mgmtGetUserFromConn(ahandle);
  if (pUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  int32_t code;
  if (pUser->superAuth) {
    SCreateUserMsg *pCreate = pCont;
    code = mgmtCreateUser(pUser->pAcct, pCreate->user, pCreate->pass);
    if (code == TSDB_CODE_SUCCESS) {
      mLPrint("user:%s is created by %s", pCreate->user, pUser->user);
    }
  } else {
    code = TSDB_CODE_NO_RIGHTS;
  }

  rpcSendResponse(ahandle, code, NULL, 0);
  return code;
}

int32_t mgmtProcessAlterUserMsg(void *pCont, int32_t contLen, void *ahandle) {
  if (mgmtCheckRedirectMsg(ahandle) != 0) {
    return TSDB_CODE_REDIRECT;
  }

  SUserObj *pOperUser = mgmtGetUserFromConn(ahandle);
  if (pOperUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  SAlterUserMsg *pAlter = pCont;
  SUserObj *pUser = mgmtGetUser(pAlter->user);
  if (pUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  if (strcmp(pUser->user, "monitor") == 0 || (strcmp(pUser->user + 1, pUser->acct) == 0 && pUser->user[0] == '_')) {
    rpcSendResponse(ahandle, TSDB_CODE_NO_RIGHTS, NULL, 0);
    return TSDB_CODE_NO_RIGHTS;
  }

  int code;
  if ((pAlter->flag & TSDB_ALTER_USER_PASSWD) != 0) {
    bool hasRight = false;
    if (strcmp(pOperUser->user, "root") == 0) {
      hasRight = true;
    } else if (strcmp(pUser->user, pOperUser->user) == 0) {
      hasRight = true;
    } else if (pOperUser->superAuth) {
      if (strcmp(pUser->user, "root") == 0) {
        hasRight = false;
      } else if (strcmp(pOperUser->acct, pUser->acct) != 0) {
        hasRight = false;
      } else {
        hasRight = true;
      }
    }

    if (hasRight) {
      memset(pUser->pass, 0, sizeof(pUser->pass));
      taosEncryptPass((uint8_t*)pAlter->pass, strlen(pAlter->pass), pUser->pass);
      code = mgmtUpdateUser(pUser);
      mLPrint("user:%s password is altered by %s, code:%d", pAlter->user, pUser->user, code);
    } else {
      code = TSDB_CODE_NO_RIGHTS;
    }

    rpcSendResponse(ahandle, code, NULL, 0);
    return code;
  }

  if ((pAlter->flag & TSDB_ALTER_USER_PRIVILEGES) != 0) {
    bool hasRight = false;

    if (strcmp(pUser->user, "root") == 0) {
      hasRight = false;
    } else if (strcmp(pUser->user, pUser->acct) == 0) {
      hasRight = false;
    } else if (strcmp(pOperUser->user, "root") == 0) {
      hasRight = true;
    } else if (strcmp(pUser->user, pOperUser->user) == 0) {
      hasRight = false;
    } else if (pOperUser->superAuth) {
      if (strcmp(pUser->user, "root") == 0) {
        hasRight = false;
      } else if (strcmp(pOperUser->acct, pUser->acct) != 0) {
        hasRight = false;
      } else {
        hasRight = true;
      }
    }

    if (pAlter->privilege == 1) { // super
      hasRight = false;
    }

    if (hasRight) {
      //if (pAlter->privilege == 1) {  // super
      //  pUser->superAuth = 1;
      //  pUser->writeAuth = 1;
      //}
      if (pAlter->privilege == 2) {  // read
        pUser->superAuth = 0;
        pUser->writeAuth = 0;
      }
      if (pAlter->privilege == 3) {  // write
        pUser->superAuth = 0;
        pUser->writeAuth = 1;
      }

      code = mgmtUpdateUser(pUser);
      mLPrint("user:%s privilege is altered by %s, code:%d", pAlter->user, pUser->user, code);
    } else {
      code = TSDB_CODE_NO_RIGHTS;
    }

    rpcSendResponse(ahandle, code, NULL, 0);
    return code;
  }

  code = TSDB_CODE_NO_RIGHTS;
  rpcSendResponse(ahandle, code, NULL, 0);
  return code;
}

int32_t mgmtProcessDropUserMsg(void *pCont, int32_t contLen, void *ahandle) {
  if (mgmtCheckRedirectMsg(ahandle) != 0) {
    return TSDB_CODE_REDIRECT;
  }

  SUserObj *pOperUser = mgmtGetUserFromConn(ahandle);
  if (pOperUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  SDropUserMsg *pDrop = pCont;
  SUserObj *pUser = mgmtGetUser(pDrop->user);
  if (pUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  if (strcmp(pUser->user, "monitor") == 0 || (strcmp(pUser->user + 1, pUser->acct) == 0 && pUser->user[0] == '_')) {
    rpcSendResponse(ahandle, TSDB_CODE_NO_RIGHTS, NULL, 0);
    return TSDB_CODE_NO_RIGHTS;
  }

  bool hasRight = false;
  if (strcmp(pUser->user, "root") == 0) {
    hasRight = false;
  } else if (strcmp(pOperUser->user, "root") == 0) {
    hasRight = true;
  } else if (strcmp(pUser->user, pOperUser->user) == 0) {
    hasRight = false;
  } else if (pOperUser->superAuth) {
    if (strcmp(pUser->user, "root") == 0) {
      hasRight = false;
    } else if (strcmp(pOperUser->acct, pUser->acct) != 0) {
      hasRight = false;
    } else {
      hasRight = true;
    }
  }

  int32_t code;
  if (hasRight) {
    code = mgmtDropUser(pUser->pAcct, pDrop->user);
    if (code == TSDB_CODE_SUCCESS) {
      mLPrint("user:%s is dropped by %s", pDrop->user, pUser->user);
    }
  } else {
    code = TSDB_CODE_NO_RIGHTS;
  }

  rpcSendResponse(ahandle, code, NULL, 0);
  return code;
}

int32_t mgmtProcessDropDbMsg(void *pCont, int32_t contLen, void *ahandle) {
  if (mgmtCheckRedirectMsg(ahandle) != 0) {
    return TSDB_CODE_REDIRECT;
  }

  SUserObj *pUser = mgmtGetUserFromConn(ahandle);
  if (pUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  int32_t code;
  if (pUser->superAuth) {
    SDropDbMsg *pDrop = pCont;
    code = mgmtDropDbByName(pUser->pAcct, pDrop->db, pDrop->ignoreNotExists);
    if (code == TSDB_CODE_SUCCESS) {
      mLPrint("DB:%s is dropped by %s", pDrop->db, pUser->user);
    }
  } else {
    code = TSDB_CODE_NO_RIGHTS;
  }

  rpcSendResponse(ahandle, code, NULL, 0);
  return code;
}

static void mgmtInitShowMsgFp() {
  mgmtGetMetaFp = (GetMateFp *)malloc(TSDB_MGMT_TABLE_MAX * sizeof(GetMateFp));
  mgmtGetMetaFp[TSDB_MGMT_TABLE_ACCT]    = mgmtGetAcctMeta;
  mgmtGetMetaFp[TSDB_MGMT_TABLE_USER]    = mgmtGetUserMeta;
  mgmtGetMetaFp[TSDB_MGMT_TABLE_DB]      = mgmtGetDbMeta;
  mgmtGetMetaFp[TSDB_MGMT_TABLE_TABLE]   = mgmtGetShowTableMeta;
  mgmtGetMetaFp[TSDB_MGMT_TABLE_DNODE]   = mgmtGetDnodeMeta;
  mgmtGetMetaFp[TSDB_MGMT_TABLE_MNODE]   = mgmtGetMnodeMeta;
  mgmtGetMetaFp[TSDB_MGMT_TABLE_VGROUP]  = mgmtGetVgroupMeta;
  mgmtGetMetaFp[TSDB_MGMT_TABLE_METRIC]  = mgmtGetShowSuperTableMeta;
  mgmtGetMetaFp[TSDB_MGMT_TABLE_MODULE]  = mgmtGetModuleMeta;
  mgmtGetMetaFp[TSDB_MGMT_TABLE_QUERIES] = mgmtGetQueryMeta;
  mgmtGetMetaFp[TSDB_MGMT_TABLE_STREAMS] = mgmtGetStreamMeta;
  mgmtGetMetaFp[TSDB_MGMT_TABLE_CONFIGS] = mgmtGetConfigMeta;
  mgmtGetMetaFp[TSDB_MGMT_TABLE_CONNS]   = mgmtGetConnsMeta;
  mgmtGetMetaFp[TSDB_MGMT_TABLE_SCORES]  = mgmtGetScoresMeta;
  mgmtGetMetaFp[TSDB_MGMT_TABLE_GRANTS]  = mgmtGetGrantsMeta;
  mgmtGetMetaFp[TSDB_MGMT_TABLE_VNODES]  = mgmtGetVnodeMeta;

  mgmtRetrieveFp = (RetrieveMetaFp *)malloc(TSDB_MGMT_TABLE_MAX * sizeof(RetrieveMetaFp));
  mgmtRetrieveFp[TSDB_MGMT_TABLE_ACCT]    = mgmtRetrieveAccts;
  mgmtRetrieveFp[TSDB_MGMT_TABLE_USER]    = mgmtRetrieveUsers;
  mgmtRetrieveFp[TSDB_MGMT_TABLE_DB]      = mgmtRetrieveDbs;
  mgmtRetrieveFp[TSDB_MGMT_TABLE_TABLE]   = mgmtRetrieveShowTables;
  mgmtRetrieveFp[TSDB_MGMT_TABLE_DNODE]   = mgmtRetrieveDnodes;
  mgmtRetrieveFp[TSDB_MGMT_TABLE_MNODE]   = mgmtRetrieveMnodes;
  mgmtRetrieveFp[TSDB_MGMT_TABLE_VGROUP]  = mgmtRetrieveVgroups;
  mgmtRetrieveFp[TSDB_MGMT_TABLE_METRIC]  = mgmtRetrieveShowSuperTables;
  mgmtRetrieveFp[TSDB_MGMT_TABLE_MODULE]  = mgmtRetrieveModules;
  mgmtRetrieveFp[TSDB_MGMT_TABLE_QUERIES] = mgmtRetrieveQueries;
  mgmtRetrieveFp[TSDB_MGMT_TABLE_STREAMS] = mgmtRetrieveStreams;
  mgmtRetrieveFp[TSDB_MGMT_TABLE_CONFIGS] = mgmtRetrieveConfigs;
  mgmtRetrieveFp[TSDB_MGMT_TABLE_CONNS]   = mgmtRetrieveConns;
  mgmtRetrieveFp[TSDB_MGMT_TABLE_SCORES]  = mgmtRetrieveScores;
  mgmtRetrieveFp[TSDB_MGMT_TABLE_GRANTS]  = mgmtRetrieveGrants;
  mgmtRetrieveFp[TSDB_MGMT_TABLE_VNODES]  = mgmtRetrieveVnodes;
}

int32_t mgmtProcessShowMsg(void *pCont, int32_t contLen, void *ahandle) {
  SShowMsg *pShowMsg = pCont;
  if (pShowMsg->type == TSDB_MGMT_TABLE_DNODE || TSDB_MGMT_TABLE_GRANTS || TSDB_MGMT_TABLE_SCORES) {
    if (mgmtCheckRedirectMsg(ahandle) != 0) {
      return TSDB_CODE_REDIRECT;
    }
  }

  int32_t  size = sizeof(SShowRsp) + sizeof(SSchema) * TSDB_MAX_COLUMNS + TSDB_EXTRA_PAYLOAD_SIZE;
  SShowRsp *pShowRsp = rpcMallocCont(size);
  if (pShowRsp == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_SERV_OUT_OF_MEMORY, NULL, 0);
    return TSDB_CODE_SERV_OUT_OF_MEMORY;
  }

  int32_t code;
  if (pShowMsg->type >= TSDB_MGMT_TABLE_MAX) {
    code = TSDB_CODE_INVALID_MSG_TYPE;
  } else {
    SShowObj *pShow = (SShowObj *) calloc(1, sizeof(SShowObj) + htons(pShowMsg->payloadLen));
    pShow->signature = pShow;
    pShow->type      = pShowMsg->type;
    strcpy(pShow->db, pShow->db);
    mTrace("pShow:%p is allocated", pShow);

    // set the table name query condition
    pShow->payloadLen = htons(pShowMsg->payloadLen);
    memcpy(pShow->payload, pShowMsg->payload, pShow->payloadLen);

    mgmtSaveQhandle(pShow);
    pShowRsp->qhandle = htobe64((uint64_t) pShow);
    code = (*mgmtGetMetaFp[(uint8_t) pShowMsg->type])(&pShowRsp->tableMeta, pShow, ahandle);
    if (code == 0) {
      size = sizeof(SShowRsp) + sizeof(SSchema) * pShow->numOfColumns;
    } else {
      mError("pShow:%p, type:%d %s, failed to get Meta, code:%d", pShow, pShowMsg->type,
             taosMsg[(uint8_t) pShowMsg->type], code);
      free(pShow);
    }
  }

  rpcSendResponse(ahandle, code, pShowRsp, size);
  return code;
}

int32_t mgmtProcessRetrieveMsg(void *pCont, int32_t contLen, void *ahandle) {
  int32_t rowsToRead = 0;
  int32_t size = 0;
  int32_t rowsRead = 0;
  SRetrieveTableMsg *pRetrieve = (SRetrieveTableMsg *)pCont;
  pRetrieve->qhandle = htobe64(pRetrieve->qhandle);

  /*
   * in case of server restart, apps may hold qhandle created by server before
   * restart, which is actually invalid, therefore, signature check is required.
   */
  if (!mgmtCheckQhandle(pRetrieve->qhandle)) {
    mError("retrieve:%p, qhandle:%p is invalid", pRetrieve, pRetrieve->qhandle);
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_QHANDLE, NULL, 0);
    return TSDB_CODE_INVALID_QHANDLE;
  }

  SShowObj *pShow = (SShowObj *)pRetrieve->qhandle;
  if (pShow->signature != (void *)pShow) {
    mError("pShow:%p, signature:%p, query memory is corrupted", pShow, pShow->signature);
    rpcSendResponse(ahandle, TSDB_CODE_MEMORY_CORRUPTED, NULL, 0);
    return TSDB_CODE_MEMORY_CORRUPTED;
  } else {
    if ((pRetrieve->free & TSDB_QUERY_TYPE_FREE_RESOURCE) != TSDB_QUERY_TYPE_FREE_RESOURCE) {
      rowsToRead = pShow->numOfRows - pShow->numOfReads;
    }

    /* return no more than 100 meters in one round trip */
    if (rowsToRead > 100) rowsToRead = 100;

    /*
     * the actual number of table may be larger than the value of pShow->numOfRows, if a query is
     * issued during a continuous create table operation. Therefore, rowToRead may be less than 0.
     */
    if (rowsToRead < 0) rowsToRead = 0;
    size = pShow->rowSize * rowsToRead;
  }

  size += 100;
  SRetrieveTableRsp *pRsp = rpcMallocCont(size);

  // if free flag is set, client wants to clean the resources
  if ((pRetrieve->free & TSDB_QUERY_TYPE_FREE_RESOURCE) != TSDB_QUERY_TYPE_FREE_RESOURCE)
    rowsRead = (*mgmtRetrieveFp[(uint8_t) pShow->type])(pShow, pRsp->data, rowsToRead, ahandle);

  if (rowsRead < 0) {
    rowsRead = 0;  // TSDB_CODE_ACTION_IN_PROGRESS;
    rpcFreeCont(pRsp);
    return TSDB_CODE_ACTION_IN_PROGRESS;
  }

  pRsp->numOfRows = htonl(rowsRead);
  pRsp->precision = htonl(TSDB_TIME_PRECISION_MILLI);  // millisecond time precision

  rpcSendResponse(ahandle, TSDB_CODE_SUCCESS, pRsp, size);

  if (rowsToRead == 0) {
    mgmtFreeQhandle(pShow);
  }

  return TSDB_CODE_SUCCESS;
}

int32_t mgmtProcessCreateTableMsg(void *pCont, int32_t contLen, void *ahandle) {
  if (mgmtCheckRedirectMsg(ahandle) != 0) {
    return TSDB_CODE_REDIRECT;
  }

  SUserObj *pUser = mgmtGetUserFromConn(ahandle);
  if (pUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  SCreateTableMsg *pCreate = (SCreateTableMsg *) pCont;
  SSchema *pSchema;
  int32_t code;

  if (!pUser->writeAuth) {
    code = TSDB_CODE_NO_RIGHTS;
  } else {
    pCreate->numOfColumns = htons(pCreate->numOfColumns);
    pCreate->numOfTags = htons(pCreate->numOfTags);
    pCreate->sqlLen = htons(pCreate->sqlLen);
    pSchema = pCreate->schema;
    for (int32_t i = 0; i < pCreate->numOfColumns + pCreate->numOfTags; ++i) {
      pSchema->bytes = htons(pSchema->bytes);
      pSchema->colId = i;
      pSchema++;
    }

    SDbObj *pDb = mgmtGetDb(pCreate->db);
    if (pDb) {
      code = mgmtCreateTable(pDb, pCreate);
      if (code == TSDB_CODE_TABLE_ALREADY_EXIST) {
        if (pCreate->igExists) {
          code = TSDB_CODE_SUCCESS;
        }
      }
    } else {
      code = TSDB_CODE_DB_NOT_SELECTED;
    }
  }

  if (code != TSDB_CODE_SUCCESS) {
    rpcSendResponse(ahandle, TSDB_CODE_SUCCESS, NULL, 0);
  }

  return code;
}

int32_t mgmtProcessDropTableMsg(void *pCont, int32_t contLen, void *ahandle) {
  if (mgmtCheckRedirectMsg(ahandle) != 0) {
    return TSDB_CODE_REDIRECT;
  }

  SUserObj *pUser = mgmtGetUserFromConn(ahandle);
  if (pUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  SDropTableMsg *pDrop = (SDropTableMsg *) pCont;
  int32_t code;

  if (!pUser->writeAuth) {
    code = TSDB_CODE_NO_RIGHTS;
  } else {
    SDbObj *pDb = mgmtGetDb(pDrop->db);
    if (pDb) {
      code = mgmtDropTable(pDb, pDrop->tableId, pDrop->igNotExists);
      if (code == TSDB_CODE_SUCCESS) {
        mTrace("table:%s is dropped by user:%s", pDrop->tableId, pUser->user);
      }
    } else {
      code = TSDB_CODE_DB_NOT_SELECTED;
    }
  }

  if (code != TSDB_CODE_SUCCESS) {
    rpcSendResponse(ahandle, code, NULL, 0);
  }
  return code;
}

int32_t mgmtProcessAlterTableMsg(void *pCont, int32_t contLen, void *ahandle) {
  if (mgmtCheckRedirectMsg(ahandle) != 0) {
    return TSDB_CODE_REDIRECT;
  }

  SUserObj *pUser = mgmtGetUserFromConn(ahandle);
  if (pUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  SAlterTableMsg *pAlter = (SAlterTableMsg *) pCont;
  int32_t code;

  if (!pUser->writeAuth) {
    code = TSDB_CODE_NO_RIGHTS;
  } else {
    pAlter->type      = htons(pAlter->type);
    pAlter->numOfCols = htons(pAlter->numOfCols);

    if (pAlter->numOfCols > 2) {
      mError("table:%s error numOfCols:%d in alter table", pAlter->tableId, pAlter->numOfCols);
      code = TSDB_CODE_APP_ERROR;
    } else {
      SDbObj *pDb = mgmtGetDb(pAlter->db);
      if (pDb) {
        for (int32_t i = 0; i < pAlter->numOfCols; ++i) {
          pAlter->schema[i].bytes = htons(pAlter->schema[i].bytes);
        }

        code = mgmtAlterTable(pDb, pAlter);
        if (code == 0) {
          mLPrint("table:%s is altered by %s", pAlter->tableId, pUser->user);
        }
      } else {
        code = TSDB_CODE_DB_NOT_SELECTED;
      }
    }
  }

  if (code != TSDB_CODE_SUCCESS) {
    rpcSendResponse(ahandle, code, NULL, 0);
  }
  return code;
}

int32_t mgmtProcessCfgDnodeMsg(void *pCont, int32_t contLen, void *ahandle) {
  if (mgmtCheckRedirectMsg(ahandle) != 0) {
    return TSDB_CODE_REDIRECT;
  }

  SUserObj *pUser = mgmtGetUserFromConn(ahandle);
  if (pUser == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_INVALID_USER, NULL, 0);
    return TSDB_CODE_INVALID_USER;
  }

  SCfgDnodeMsg *pCfg = (SCfgDnodeMsg *)pCont;
  int32_t code;

  if (strcmp(pUser->pAcct->user, "root") != 0) {
    code = TSDB_CODE_NO_RIGHTS;
  } else {
    code = mgmtSendCfgDnodeMsg(pCont);
  }

  if (code == TSDB_CODE_SUCCESS) {
    mTrace("dnode:%s is configured by %s", pCfg->ip, pUser->user);
  }

  rpcSendResponse(ahandle, code, NULL, 0);
  return code;
}

int32_t mgmtProcessHeartBeatMsg(void *pCont, int32_t contLen, void *ahandle) {
  SHeartBeatMsg *pHBMsg = (SHeartBeatMsg *) pCont;
  mgmtSaveQueryStreamList(pHBMsg);

  SHeartBeatRsp *pHBRsp = (SHeartBeatRsp *) rpcMallocCont(contLen);
  if (pHBRsp == NULL) {
    rpcSendResponse(ahandle, TSDB_CODE_SERV_OUT_OF_MEMORY, NULL, 0);
    rpcFreeCont(pCont);
    return TSDB_CODE_SERV_OUT_OF_MEMORY;
  }

  SRpcConnInfo connInfo;
  rpcGetConnInfo(ahandle, &connInfo);

  pHBRsp->ipList.inUse = 0;
  pHBRsp->ipList.port = htons(tsMgmtShellPort);
  pHBRsp->ipList.numOfIps = 0;
  if (pSdbPublicIpList != NULL && pSdbIpList != NULL) {
    pHBRsp->ipList.numOfIps = htons(pSdbPublicIpList->numOfIps);
    if (connInfo.serverIp == tsPublicIpInt) {
      for (int i = 0; i < pSdbPublicIpList->numOfIps; ++i) {
        pHBRsp->ipList.ip[i] = htonl(pSdbPublicIpList->ip[i]);
      }
    } else {
      for (int i = 0; i < pSdbIpList->numOfIps; ++i) {
        pHBRsp->ipList.ip[i] = htonl(pSdbIpList->ip[i]);
      }
    }
  }

  /*
   * TODO
   * Dispose kill stream or kill query message
   */
  pHBRsp->queryId = 0;
  pHBRsp->streamId = 0;
  pHBRsp->killConnection = 0;

  rpcSendResponse(ahandle, TSDB_CODE_SUCCESS, pHBRsp, sizeof(SHeartBeatMsg));
  return TSDB_CODE_SUCCESS;
}

int32_t mgmtRetriveUserAuthInfo(char *user, char *spi, char *encrypt, char *secret, char *ckey) {
  *spi = 0;
  *encrypt = 0;
  *ckey = 0;

  SUserObj *pUser = mgmtGetUser(user);
  if (pUser == NULL) {
    *secret = 0;
    return TSDB_CODE_INVALID_USER;
  } else {
    memcpy(secret, pUser->pass, TSDB_KEY_LEN);
    return TSDB_CODE_SUCCESS;
  }
}

static int32_t mgmtProcessConnectMsg(void *pCont, int32_t contLen, void *thandle) {
  SConnectMsg *pConnectMsg = (SConnectMsg *) pCont;
  SRpcConnInfo connInfo;
  rpcGetConnInfo(thandle, &connInfo);
  int32_t code;

  SUserObj *pUser = mgmtGetUser(connInfo.user);
  if (pUser == NULL) {
    code = TSDB_CODE_INVALID_USER;
    goto connect_over;
  }

  if (mgmtCheckExpired()) {
    code = TSDB_CODE_GRANT_EXPIRED;
    goto connect_over;
  }

  SAcctObj *pAcct = mgmtGetAcct(pUser->acct);
  if (pAcct == NULL) {
    code = TSDB_CODE_INVALID_ACCT;
    goto connect_over;
  }

  code = taosCheckVersion(pConnectMsg->clientVersion, version, 3);
  if (code != TSDB_CODE_SUCCESS) {
    goto connect_over;
  }

  if (pConnectMsg->db[0]) {
    char dbName[TSDB_TABLE_ID_LEN] = {0};
    sprintf(dbName, "%x%s%s", pAcct->acctId, TS_PATH_DELIMITER, pConnectMsg->db);
    SDbObj *pDb = mgmtGetDb(dbName);
    if (pDb == NULL) {
      code = TSDB_CODE_INVALID_DB;
      goto connect_over;
    }
  }

  SConnectRsp *pConnectRsp = rpcMallocCont(sizeof(SConnectRsp));
  if (pConnectRsp == NULL) {
    code = TSDB_CODE_SERV_OUT_OF_MEMORY;
    goto connect_over;
  }

  sprintf(pConnectRsp->acctId, "%x", pAcct->acctId);
  strcpy(pConnectRsp->serverVersion, version);
  pConnectRsp->writeAuth = pUser->writeAuth;
  pConnectRsp->superAuth = pUser->superAuth;
  pConnectRsp->ipList.inUse = 0;
  pConnectRsp->ipList.port = htons(tsMgmtShellPort);
  pConnectRsp->ipList.numOfIps = 0;
  if (pSdbPublicIpList != NULL && pSdbIpList != NULL) {
    pConnectRsp->ipList.numOfIps = htons(pSdbPublicIpList->numOfIps);
    if (connInfo.serverIp == tsPublicIpInt) {
      for (int i = 0; i < pSdbPublicIpList->numOfIps; ++i) {
        pConnectRsp->ipList.ip[i] = htonl(pSdbPublicIpList->ip[i]);
      }
    } else {
      for (int i = 0; i < pSdbIpList->numOfIps; ++i) {
        pConnectRsp->ipList.ip[i] = htonl(pSdbIpList->ip[i]);
      }
    }
  }

connect_over:
  if (code != TSDB_CODE_SUCCESS) {
    mLError("user:%s login from %s, code:%d", connInfo.user, taosIpStr(connInfo.clientIp), code);
    rpcSendResponse(thandle, code, NULL, 0);
  } else {
    mLPrint("user:%s login from %s, code:%d", connInfo.user, taosIpStr(connInfo.clientIp), code);
    rpcSendResponse(thandle, code, pConnectRsp, sizeof(SConnectRsp));
  }

  return code;
}

/**
 * check if we need to add mgmtProcessTableMetaMsg into tranQueue, which will be executed one-by-one.
 */
static bool mgmtCheckMeterMetaMsgType(void *pMsg) {
  STableInfoMsg *pInfo = (STableInfoMsg *) pMsg;
  int16_t autoCreate = htons(pInfo->createFlag);
  STableInfo *pTable = mgmtGetTable(pInfo->tableId);

  // If table does not exists and autoCreate flag is set, we add the handler into task queue
  bool addIntoTranQueue = (pTable == NULL && autoCreate == 1);
  if (addIntoTranQueue) {
    mTrace("table:%s auto created task added", pInfo->tableId);
  }

  return addIntoTranQueue;
}

static bool mgmtCheckMsgReadOnly(int8_t type, void *pCont) {
  if ((type == TSDB_MSG_TYPE_TABLE_META && (!mgmtCheckMeterMetaMsgType(pCont)))  ||
       type == TSDB_MSG_TYPE_STABLE_META || type == TSDB_MSG_TYPE_RETRIEVE ||
       type == TSDB_MSG_TYPE_SHOW || type == TSDB_MSG_TYPE_MULTI_TABLE_META      ||
       type == TSDB_MSG_TYPE_CONNECT) {
    return true;
  }

  return false;
}

static void mgmtProcessMsgFromShell(char type, void *pCont, int contLen, void *ahandle, int32_t code) {
  if (sdbGetRunStatus() != SDB_STATUS_SERVING) {
    mTrace("shell msg is ignored since SDB is not ready");
    rpcSendResponse(ahandle, TSDB_CODE_NOT_READY, NULL, 0);
    rpcFreeCont(pCont);
    return;
  }

  if (mgmtCheckMsgReadOnly(type, pCont)) {
    (*mgmtProcessShellMsg[(int8_t)type])(pCont, contLen, ahandle);
  } else {
    if (mgmtProcessShellMsg[(int8_t)type]) {
      mgmtAddToTranRequest((int8_t)type, pCont, contLen, ahandle);
    } else {
      mError("%s from shell is not processed", taosMsg[(int8_t)type]);
    }
  }

  //TODO free may be cause segment fault
  //
  // rpcFreeCont(pCont);
}

void mgmtInitProcessShellMsg() {
  mgmtProcessShellMsg[TSDB_MSG_TYPE_CONNECT]          = mgmtProcessConnectMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_HEARTBEAT]        = mgmtProcessHeartBeatMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_CREATE_DB]        = mgmtProcessCreateDbMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_ALTER_DB]         = mgmtProcessAlterDbMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_DROP_DB]          = mgmtProcessDropDbMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_USE_DB]           = mgmtProcessUnSupportMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_CREATE_USER]      = mgmtProcessCreateUserMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_ALTER_USER]       = mgmtProcessAlterUserMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_DROP_USER]        = mgmtProcessDropUserMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_CREATE_ACCT]      = mgmtProcessCreateAcctMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_DROP_ACCT]        = mgmtProcessDropAcctMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_ALTER_ACCT]       = mgmtProcessAlterAcctMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_CREATE_TABLE]     = mgmtProcessCreateTableMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_DROP_TABLE]       = mgmtProcessDropTableMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_ALTER_TABLE]      = mgmtProcessAlterTableMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_CREATE_DNODE]     = mgmtProcessCreateDnodeMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_DROP_DNODE]       = mgmtProcessDropDnodeMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_DNODE_CFG]        = mgmtProcessCfgDnodeMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_CREATE_MNODE]     = mgmtProcessUnSupportMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_DROP_MNODE]       = mgmtProcessDropMnodeMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_CFG_MNODE]        = mgmtProcessCfgMnodeMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_KILL_QUERY]       = mgmtProcessKillQueryMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_KILL_STREAM]      = mgmtProcessKillStreamMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_KILL_CONNECTION]  = mgmtProcessKillConnectionMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_SHOW]             = mgmtProcessShowMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_RETRIEVE]         = mgmtProcessRetrieveMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_TABLE_META]       = mgmtProcessTableMetaMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_MULTI_TABLE_META] = mgmtProcessMultiTableMetaMsg;
  mgmtProcessShellMsg[TSDB_MSG_TYPE_STABLE_META]      = mgmtProcessSuperTableMetaMsg;
}

static int32_t mgmtCheckRedirectMsgImp(void *pConn) {
  return 0;
}

int32_t (*mgmtCheckRedirectMsg)(void *pConn) = mgmtCheckRedirectMsgImp;

static int32_t mgmtProcessUnSupportMsg(void *pCont, int32_t contLen, void *ahandle) {
  rpcSendResponse(ahandle, TSDB_CODE_OPS_NOT_SUPPORT, NULL, 0);
  return TSDB_CODE_OPS_NOT_SUPPORT;
}

int32_t (*mgmtProcessAlterAcctMsg)(void *pCont, int32_t contLen, void *ahandle)   = mgmtProcessUnSupportMsg;
int32_t (*mgmtProcessCreateDnodeMsg)(void *pCont, int32_t contLen, void *ahandle) = mgmtProcessUnSupportMsg;
int32_t (*mgmtProcessCfgMnodeMsg)(void *pCont, int32_t contLen, void *ahandle)    = mgmtProcessUnSupportMsg;
int32_t (*mgmtProcessDropMnodeMsg)(void *pCont, int32_t contLen, void *ahandle)   = mgmtProcessUnSupportMsg;
int32_t (*mgmtProcessDropDnodeMsg)(void *pCont, int32_t contLen, void *ahandle)   = mgmtProcessUnSupportMsg;
int32_t (*mgmtProcessDropAcctMsg)(void *pCont, int32_t contLen, void *ahandle)    = mgmtProcessUnSupportMsg;
int32_t (*mgmtProcessCreateAcctMsg)(void *pCont, int32_t contLen, void *ahandle)  = mgmtProcessUnSupportMsg;