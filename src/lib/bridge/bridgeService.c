//
// FILE            bridgeService.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // malloc, free, calloc
#include <string.h>                                   // strcmp, strdup, strstr, strncmp, strlen
#include <pthread.h>                                  // pthread_mutex_t, pthread_cond_t
#include <time.h>                                     // clock_gettime

#include "corLog/corLog.h"                            // COR_T, COR_W
#include "corBridge/BridgeBroker.h"                   // BRIDGE_OK, ...
#include "corBridge/BridgeDriver.h"                   // BridgeDriver, bridges, bridgeCount

#include "db/Tenant.h"                                // Tenant, tenant0, tenantLookup
#include "bridge/bridgeSampleIn.h"                    // bridgeThreadBind
#include "bridge/bridgeService.h"                     // Own interface
#include "coraineTraceLevels.h"                       // CtService



// -----------------------------------------------------------------------------
//
// Running - an execution a bridge has, until its outcome
//
typedef struct Running
{
  char*            executionId;
  char*            tenantName;                        // NULL: the default tenant
  bool             waiting;                           // a synchronous service: a request thread waits
  bool             done;
  char             status[16];
  char*            outputJson;
  char*            errorJson;
  pthread_cond_t   cond;
  struct Running*  next;
} Running;

static pthread_mutex_t       mutex   = PTHREAD_MUTEX_INITIALIZER;
static Running*              running = NULL;
static BridgeServiceApplyFn  applyFn = NULL;



// -----------------------------------------------------------------------------
//
// schemeIn - is the URI's scheme one of a comma-separated list?
//
static bool schemeIn(const char* uri, const char* schemes)
{
  const char* sep = strstr(uri, "://");

  if (sep == NULL)
    return false;

  int         len = sep - uri;
  const char* s   = schemes;

  while ((s != NULL) && (*s != 0))
  {
    const char* comma = strchr(s, ',');
    int         sLen  = (comma != NULL) ? (int) (comma - s) : (int) strlen(s);

    if ((sLen == len) && (strncmp(s, uri, len) == 0))
      return true;

    s = (comma != NULL) ? &comma[1] : NULL;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// serviceBridge - the loaded bridge that executes services of this URI's scheme, or NULL
//
static BridgeDriver* serviceBridge(const char* url)
{
  for (int i = 0; i < bridgeCount; i++)
  {
    BridgeDriver* driverP = &bridges[i];

    if ((driverP->abiVersion < 11) || (driverP->serviceSchemes == NULL) || (driverP->serviceExecute == NULL))
      continue;

    if (schemeIn(url, driverP->serviceSchemes) == true)
      return driverP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// runningFind / runningRemove - under the mutex
//
static Running* runningFind(const char* executionId)
{
  for (Running* rP = running; rP != NULL; rP = rP->next)
  {
    if (strcmp(rP->executionId, executionId) == 0)
      return rP;
  }

  return NULL;
}

static void runningRemove(Running* rP)
{
  Running** pP = &running;

  while ((*pP != NULL) && (*pP != rP))
    pP = &(*pP)->next;

  if (*pP == rP)
    *pP = rP->next;
}

static void runningFree(Running* rP)
{
  pthread_cond_destroy(&rP->cond);
  free(rP->executionId);
  free(rP->tenantName);
  free(rP->outputJson);
  free(rP->errorJson);
  free(rP);
}



// -----------------------------------------------------------------------------
//
// bridgeServiceHas -
//
bool bridgeServiceHas(const char* url)
{
  return serviceBridge(url) != NULL;
}



// -----------------------------------------------------------------------------
//
// bridgeServiceApplySet -
//
void bridgeServiceApplySet(BridgeServiceApplyFn fn)
{
  applyFn = fn;
}



// -----------------------------------------------------------------------------
//
// bridgeServiceResultRelease -
//
void bridgeServiceResultRelease(BridgeServiceResult* resultP)
{
  free(resultP->outputJson);
  free(resultP->errorJson);
  resultP->outputJson = NULL;
  resultP->errorJson  = NULL;
}



// -----------------------------------------------------------------------------
//
// bridgeServiceExecute -
//
int bridgeServiceExecute(const char* url, const char* executionId, const char* tenantName, const char* inputJson, int waitMs, BridgeServiceResult* resultP)
{
  BridgeDriver* driverP = serviceBridge(url);

  memset(resultP, 0, sizeof(BridgeServiceResult));

  if (driverP == NULL)
    return BRIDGE_NOT_FOUND;

  //
  // Known BEFORE it is handed over: the bridge may report before serviceExecute returns
  //
  Running* rP = (Running*) calloc(1, sizeof(Running));

  if (rP == NULL)
    return BRIDGE_ERR;

  rP->executionId = strdup(executionId);
  rP->tenantName  = ((tenantName != NULL) && (tenantName[0] != 0)) ? strdup(tenantName) : NULL;
  rP->waiting     = (waitMs > 0);
  pthread_cond_init(&rP->cond, NULL);

  pthread_mutex_lock(&mutex);
  rP->next = running;
  running  = rP;
  pthread_mutex_unlock(&mutex);

  int r = driverP->serviceExecute(url, executionId, inputJson);

  COR_T(CtService, "bridge '%s': serviceExecute(%s, %s) - %d", driverP->alias, url, executionId, r);

  if (r != BRIDGE_OK)
  {
    pthread_mutex_lock(&mutex);
    runningRemove(rP);
    pthread_mutex_unlock(&mutex);
    runningFree(rP);
    return r;
  }

  if (waitMs <= 0)
    return BRIDGE_OK;

  //
  // A synchronous service: its outcome, or the deadline
  //
  struct timespec until;

  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec  += waitMs / 1000;
  until.tv_nsec += (long) (waitMs % 1000) * 1000000L;
  if (until.tv_nsec >= 1000000000L)
  {
    until.tv_sec  += 1;
    until.tv_nsec -= 1000000000L;
  }

  pthread_mutex_lock(&mutex);

  while ((rP->done == false) && (pthread_cond_timedwait(&rP->cond, &mutex, &until) == 0))
    ;

  runningRemove(rP);
  pthread_mutex_unlock(&mutex);

  if (rP->done == true)
  {
    memcpy(resultP->status, rP->status, sizeof(resultP->status));
    resultP->outputJson = rP->outputJson;
    resultP->errorJson  = rP->errorJson;
    rP->outputJson      = NULL;
    rP->errorJson       = NULL;
  }
  else if (driverP->serviceCancel != NULL)
    driverP->serviceCancel(url, executionId);         // no outcome in time: not left running, where the bridge can stop it

  runningFree(rP);
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// bridgeServiceCancel -
//
int bridgeServiceCancel(const char* url, const char* executionId)
{
  BridgeDriver* driverP = serviceBridge(url);

  if ((driverP == NULL) || (driverP->serviceCancel == NULL))
    return BRIDGE_UNSUPPORTED;

  int r = driverP->serviceCancel(url, executionId);

  COR_T(CtService, "bridge '%s': serviceCancel(%s, %s) - %d", driverP->alias, url, executionId, r);

  if (r == BRIDGE_OK)
  {
    pthread_mutex_lock(&mutex);
    Running* rP = runningFind(executionId);
    if (rP != NULL)
      runningRemove(rP);
    pthread_mutex_unlock(&mutex);

    if (rP != NULL)
      runningFree(rP);
  }

  return r;
}



// -----------------------------------------------------------------------------
//
// bridgeServiceUpdateIn -
//
int bridgeServiceUpdateIn(const char* bridgeName, const char* executionId, const char* status, const char* progressJson, const char* outputJson, const char* errorJson)
{
  bool terminal = (status != NULL) && ((strcmp(status, "completed") == 0) || (strcmp(status, "failed") == 0));

  if ((executionId == NULL) || ((status != NULL) && (terminal == false) && (strcmp(status, "executing") != 0)))
    return BRIDGE_BAD_INPUT;

  pthread_mutex_lock(&mutex);

  Running* rP = runningFind(executionId);

  if (rP == NULL)
  {
    pthread_mutex_unlock(&mutex);
    COR_W("bridge '%s': a report on Service Execution '%s', which it does not run", (bridgeName != NULL) ? bridgeName : "?", executionId);
    return BRIDGE_NOT_FOUND;
  }

  if (rP->waiting == true)
  {
    //
    // A synchronous service: only its outcome matters - the waiting request applies it
    //
    if (terminal == true)
    {
      snprintf(rP->status, sizeof(rP->status), "%s", status);
      rP->outputJson = (outputJson != NULL) ? strdup(outputJson) : NULL;
      rP->errorJson  = (errorJson  != NULL) ? strdup(errorJson)  : NULL;
      rP->done       = true;
      pthread_cond_signal(&rP->cond);
    }

    pthread_mutex_unlock(&mutex);
    return BRIDGE_OK;
  }

  //
  // An asynchronous one: applied here, on the plugin's thread, as the execution's tenant
  //
  char* tenantName = (rP->tenantName != NULL) ? strdup(rP->tenantName) : NULL;

  if (terminal == true)
    runningRemove(rP);

  pthread_mutex_unlock(&mutex);

  if (terminal == true)
    runningFree(rP);

  Tenant* tenantP = (tenantName != NULL) ? tenantLookup(tenantName) : &tenant0;

  free(tenantName);

  if ((tenantP == NULL) || (applyFn == NULL))
    return BRIDGE_NOT_FOUND;

  bridgeThreadBind(tenantP);
  applyFn(executionId, status, progressJson, outputJson, errorJson);

  return BRIDGE_OK;
}
