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

#include "corBase/corUriPasswordMask.h"               // corUriPasswordMask
#include "corLog/corLog.h"                            // COR_T, COR_W
#include "corBridge/BridgeBroker.h"                   // BRIDGE_OK, ...
#include "corBridge/BridgeDriver.h"                   // BridgeDriver, bridges, bridgeCount

#include "db/Tenant.h"                                // Tenant, tenant0, tenantLookup
#include "bridge/bridgeSampleIn.h"                    // bridgeThreadBind
#include "bridge/bridgeService.h"                     // Own interface
#include "coraineTraceLevels.h"                       // CtService



// -----------------------------------------------------------------------------
//
// Deferred - a report a bridge made from inside serviceExecute, on the thread that called it
//
typedef struct Deferred
{
  char*            status;
  char*            progressJson;
  char*            outputJson;
  char*            errorJson;
  struct Deferred* next;
} Deferred;



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
  bool             inExecute;                         // the bridge's serviceExecute has not returned yet
  pthread_t        executeThread;                     // the thread that called it
  Deferred*        deferredP;                         // what the bridge reported on that thread, in order
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

static void deferredFree(Deferred* dP)
{
  while (dP != NULL)
  {
    Deferred* nextP = dP->next;

    free(dP->status);
    free(dP->progressJson);
    free(dP->outputJson);
    free(dP->errorJson);
    free(dP);
    dP = nextP;
  }
}

static char* dupOrNull(const char* s)
{
  return (s != NULL) ? strdup(s) : NULL;
}

static void runningFree(Running* rP)
{
  deferredFree(rP->deferredP);
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



static int updateApply(const char* bridgeName, const char* executionId, const char* status, const char* progressJson, const char* outputJson, const char* errorJson, bool bind);



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
  rP->waiting       = (waitMs > 0);
  rP->inExecute     = true;
  rP->executeThread = pthread_self();
  pthread_cond_init(&rP->cond, NULL);

  pthread_mutex_lock(&mutex);
  rP->next = running;
  running  = rP;
  pthread_mutex_unlock(&mutex);

  int r = driverP->serviceExecute(url, executionId, inputJson);

  char shown[1024];
  COR_T(CtService, "bridge '%s': serviceExecute(%s, %s) - %d", driverP->alias, corUriPasswordMask(url, shown, sizeof(shown)), executionId, r);

  pthread_mutex_lock(&mutex);
  Deferred* deferredP = rP->deferredP;
  rP->deferredP = NULL;
  rP->inExecute = false;
  pthread_mutex_unlock(&mutex);

  if (r != BRIDGE_OK)
  {
    deferredFree(deferredP);
    pthread_mutex_lock(&mutex);
    runningRemove(rP);
    pthread_mutex_unlock(&mutex);
    runningFree(rP);
    return r;
  }

  if (waitMs <= 0)
  {
    //
    // What the bridge reported from inside serviceExecute, applied now - in the request, as its tenant
    //
    char* id = strdup(executionId);

    for (Deferred* dP = deferredP; (dP != NULL) && (id != NULL); dP = dP->next)
      updateApply(driverP->alias, id, dP->status, dP->progressJson, dP->outputJson, dP->errorJson, false);

    free(id);
    deferredFree(deferredP);
    return BRIDGE_OK;
  }

  deferredFree(deferredP);                            // a synchronous service: its waiter reads rP->done

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

  char shown[1024];
  COR_T(CtService, "bridge '%s': serviceCancel(%s, %s) - %d", driverP->alias, corUriPasswordMask(url, shown, sizeof(shown)), executionId, r);

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
// updateApply - a bridge's report: kept when it comes from inside serviceExecute, else applied - bound to
// the execution's tenant first, unless the thread is already in that execution's request (bind false)
//
static int updateApply(const char* bridgeName, const char* executionId, const char* status, const char* progressJson, const char* outputJson, const char* errorJson, bool bind)
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

  //
  // From inside serviceExecute, on the thread that called it: that thread is in the middle of a request
  // - it is not bound to anything else, and the execution is not answered yet. Kept, and applied when
  // serviceExecute returns (bridgeServiceExecute).
  //
  if ((rP->inExecute == true) && (rP->waiting == false) && (pthread_equal(rP->executeThread, pthread_self()) != 0))
  {
    Deferred*  dP  = (Deferred*) calloc(1, sizeof(Deferred));
    Deferred** pP  = &rP->deferredP;

    if (dP == NULL)
    {
      pthread_mutex_unlock(&mutex);
      return BRIDGE_ERR;
    }

    dP->status       = dupOrNull(status);
    dP->progressJson = dupOrNull(progressJson);
    dP->outputJson   = dupOrNull(outputJson);
    dP->errorJson    = dupOrNull(errorJson);

    while (*pP != NULL)
      pP = &(*pP)->next;
    *pP = dP;

    pthread_mutex_unlock(&mutex);
    return BRIDGE_OK;
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

  if (bind == true)
    bridgeThreadBind(tenantP);
  applyFn(executionId, status, progressJson, outputJson, errorJson);

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// bridgeServiceUpdateIn -
//
int bridgeServiceUpdateIn(const char* bridgeName, const char* executionId, const char* status, const char* progressJson, const char* outputJson, const char* errorJson)
{
  return updateApply(bridgeName, executionId, status, progressJson, outputJson, errorJson, true);
}
