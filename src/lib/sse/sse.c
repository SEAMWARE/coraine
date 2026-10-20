//
// FILE            sse.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Server-Sent Events - see sse.h and doc/sse.md
//
// One entry per subscription on its stream, in one list under one mutex. A notification (corNgsild's
// hook, on the notifying thread) is written on the stream at once - corRestStreamWrite copies and never
// waits - or, before the stream is open, kept until it is. A thread of its own, once a second: is the
// client still there (a write of 0 bytes), a comment on a stream quiet for 15 s (a proxy keeps it),
// the counters of what was written (ldNotifyStreamSent - not from the notifying thread, which may hold
// the subscription cache's lock), and the subscriptions of the streams that ended, deleted.
//
#include <pthread.h>                                 // pthread_*
#include <stdint.h>                                  // uint64_t, uintptr_t
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // malloc, calloc, free
#include <string.h>                                  // strcmp, strlen, strdup, memcpy
#include <strings.h>                                 // strncasecmp
#include <time.h>                                    // clock_gettime

#include "corLog/corLog.h"                           // COR_I, COR_W, COR_E
#include "corTree/CorNode.h"                         // CorNode
#include "corRest/corRest.h"                         // corRest
#include "corRest/CorRestService.h"                  // CorRestService (ldOp)
#include "corRest/CorRestKeyValue.h"                 // CorRestKeyValue
#include "corRest/corRestOutHeader.h"                // corRestOutHeaderAdd
#include "corRest/corRestRun.h"                      // corRestRunJson
#include "corRest/corRestStream.h"                   // CorRestStream, corRestStreamResponse, corRestStreamWrite, corRestStreamEnd
#include "corNgsild/corNgsild.h"                     // ldError, LD_ERROR_*
#include "corNgsild/ldHooks.h"                       // ldAcceptPrecondition
#include "corNgsild/ldTermId.h"                      // ldTermId
#include "corNgsild/LdOp.h"                          // LdOpCreateSubscription, LdOpCreateCsourceSubscription
#include "corNgsild/LdSubCache.h"                    // LdSubCache, LdSubCacheItem
#include "corNgsild/ldSubCache.h"                    // ldSubCacheRdLock, ldSubCacheUnlock
#include "corNgsild/LdPernotCache.h"                 // LdPernotCache, LdPernotItem
#include "corNgsild/ldPernotCache.h"                 // ldPernotCacheRdLock, ldPernotCacheUnlock
#include "corNgsild/ldNotifyStream.h"                // ldNotifyStreamSet, ldNotifyIsEventStream, ldNotifyStreamSent

#if COR_FEATURE_AUTH
#include "auth/auth.h"                               // authTrustNext, authTrustClear
#endif
#include "db/Tenant.h"                               // Tenant, tenant0, tenantList, tenantApiName, tenantLookup
#include "sse/sse.h"                                 // Own interface



// -----------------------------------------------------------------------------
//
// SseEntry - one subscription on its stream
//
typedef struct SseEntry
{
  unsigned long long  id;                            // the stream's open finds it by this - never by pointer
  Tenant*             tenantP;                       // matched against the notifying thread's
  char*               tenant;                        // its API name ("" the default) - looked up again later
  char*               subId;
  bool                csrSub;
  CorRestStream*      streamP;                       // NULL until open
  bool                opened;                        // the open came (streamP may be NULL: none could be made)
  bool                ended;                         // the stream is over: the subscription is to be deleted
  bool                forgotten;                     // the subscription is deleted: nothing more to do
  char**              waitV;                         // events made before the open
  int                 waits;
  int                 written;                       // events written, not counted yet
  uint64_t            lastWriteMs;
  uint64_t            madeMs;
  struct SseEntry*    next;
} SseEntry;

#define WAIT_MAX          1000                       // events kept before the open - a client that never reads the 201 is cut
#define KEEP_ALIVE_MS     15000
#define OPEN_WAIT_MS      30000                      // a 201 never written (the client left first): its subscription goes

static pthread_mutex_t    sseMutex     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t     tickCond     = PTHREAD_COND_INITIALIZER;
static SseEntry*          entryList    = NULL;
static unsigned long long entryNext    = 1;
static int                entries      = 0;          // in entryList, not forgotten
static int                reserved     = 0;          // sseCreateCheck said yes, register or abort to come
static int                streamsMax   = 1000;
static bool               stopping     = false;
static bool               tickUp       = false;
static pthread_t          tickTid;



// -----------------------------------------------------------------------------
//
// nowMs - CLOCK_MONOTONIC in ms; nowNs - the wall clock in ns, as the subscription counters are
//
static uint64_t nowMs(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000;
}

static uint64_t nowNs(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}



// -----------------------------------------------------------------------------
//
// entryFind - the subscription's entry; called with sseMutex held
//
static SseEntry* entryFind(Tenant* tenantP, const char* subId, bool csrSub)
{
  for (SseEntry* eP = entryList; eP != NULL; eP = eP->next)
  {
    if ((eP->forgotten == false) && (eP->tenantP == tenantP) && (eP->csrSub == csrSub) && (strcmp(eP->subId, subId) == 0))
      return eP;
  }

  return NULL;
}

static SseEntry* entryById(unsigned long long id)
{
  for (SseEntry* eP = entryList; eP != NULL; eP = eP->next)
  {
    if (eP->id == id)
      return eP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// entryUnlink / entryFree - out of the list (sseMutex held), and its memory (the stream ended before)
//
static void entryUnlink(SseEntry* eP)
{
  for (SseEntry** pP = &entryList; *pP != NULL; pP = &(*pP)->next)
  {
    if (*pP == eP)
    {
      *pP = eP->next;
      return;
    }
  }
}

static void entryFree(SseEntry* eP)
{
  for (int i = 0; i < eP->waits; i++)
    free(eP->waitV[i]);

  free(eP->waitV);
  free(eP->tenant);
  free(eP->subId);
  free(eP);
}



// -----------------------------------------------------------------------------
//
// entryEnd - the stream over (sseMutex held): ended, the subscription to be deleted by the tick
//
static void entryEnd(SseEntry* eP)
{
  if (eP->streamP != NULL)
  {
    corRestStreamEnd(eP->streamP);
    eP->streamP = NULL;
  }

  eP->ended = true;
}



// -----------------------------------------------------------------------------
//
// streamWrite - one event, or a comment, on the open stream (sseMutex held); false: the stream ended
//
static bool streamWrite(SseEntry* eP, const char* text, int len)
{
  if (corRestStreamWrite(eP->streamP, text, len) == false)
  {
    COR_I("sse: the stream of '%s' ended (the client left, or fell behind) - its subscription goes", eP->subId);
    entryEnd(eP);
    return false;
  }

  eP->lastWriteMs = nowMs();
  return true;
}



// -----------------------------------------------------------------------------
//
// eventMake - "event: notification", the Notification on one data line, a blank line (malloc'd)
//
static char* eventMake(const char* json, int* lenP)
{
  static const char head[] = "event: notification\ndata: ";
  int               jlen   = strlen(json);
  int               len    = (int) sizeof(head) - 1 + jlen + 2;
  char*             text   = (char*) malloc(len + 1);

  if (text == NULL)
    return NULL;

  memcpy(text, head, sizeof(head) - 1);
  memcpy(&text[sizeof(head) - 1], json, jlen);
  memcpy(&text[sizeof(head) - 1 + jlen], "\n\n", 3);

  *lenP = len;
  return text;
}



// -----------------------------------------------------------------------------
//
// streamPut - corNgsild's hook (ldNotifyStreamSet): a Notification of a subscription on its stream
//
static bool streamPut(void* tenantP, const char* subId, bool csrSub, const char* json)
{
  int   len  = 0;
  char* text = eventMake(json, &len);

  if (text == NULL)
    return false;

  pthread_mutex_lock(&sseMutex);

  SseEntry* eP = entryFind((Tenant*) tenantP, subId, csrSub);
  bool      ok = false;

  if ((eP == NULL) || (eP->ended == true))
    ok = false;                                       // a stream that is gone: the subscription goes with it
  else if (eP->streamP != NULL)
  {
    ok = streamWrite(eP, text, len);
    if (ok)
      eP->written += 1;
  }
  else if (eP->opened == false)
  {
    if (eP->waits == WAIT_MAX)
      entryEnd(eP);                                   // the 201 not taken by the client in all this time
    else
    {
      char** v = (char**) realloc(eP->waitV, (eP->waits + 1) * sizeof(char*));

      if (v != NULL)
      {
        eP->waitV            = v;
        eP->waitV[eP->waits++] = text;
        text                 = NULL;
        ok                   = true;
      }
    }
  }

  pthread_mutex_unlock(&sseMutex);

  free(text);
  return ok;
}



// -----------------------------------------------------------------------------
//
// streamOpened - corRest's open (CorRestStreamOpen): the 201 is on its way, the stream is there
//
static void streamOpened(CorRestStream* streamP, void* ctx)
{
  pthread_mutex_lock(&sseMutex);

  SseEntry* eP = entryById((unsigned long long) (uintptr_t) ctx);

  if (eP == NULL)                                     // gone already (sseStop, or the open came too late)
  {
    pthread_mutex_unlock(&sseMutex);
    if (streamP != NULL)
      corRestStreamEnd(streamP);
    return;
  }

  eP->opened  = true;
  eP->streamP = streamP;

  if ((streamP == NULL) || (eP->forgotten == true) || (eP->ended == true) || (stopping == true))
    entryEnd(eP);
  else
  {
    //
    // ": stream" first - the headers and something to read, so that a client knows at once it is there -
    // then what was made before the open
    //
    if (streamWrite(eP, ": stream\n\n", 10) == true)
    {
      for (int i = 0; i < eP->waits; i++)
      {
        if (streamWrite(eP, eP->waitV[i], strlen(eP->waitV[i])) == false)
          break;
        eP->written += 1;
      }
    }
  }

  for (int i = 0; i < eP->waits; i++)
    free(eP->waitV[i]);
  free(eP->waitV);
  eP->waitV = NULL;
  eP->waits = 0;

  pthread_mutex_unlock(&sseMutex);
}



// -----------------------------------------------------------------------------
//
// subscriptionDelete - the subscription of a stream that ended, deleted through its own route (as a
// WebSocket's, transport.c): the cache, the store, its owner - and sseForget, which frees the entry
//
static void runIgnore(int status, CorRestKeyValue* headerV, int headers, const char* body, int bodyLen, void* ctx)
{
  (void) status; (void) headerV; (void) headers; (void) body; (void) bodyLen; (void) ctx;
}

static void subscriptionDelete(const char* tenant, const char* subId, bool csrSub)
{
  const char*     base    = csrSub ? "/ngsi-ld/v1/csourceSubscriptions/" : "/ngsi-ld/v1/subscriptions/";
  int             size    = strlen(base) + strlen(subId) + 1;
  char*           path    = (char*) malloc(size);
  CorRestKeyValue tenantH = { (char*) "NGSILD-Tenant", (char*) tenant };

  if (path == NULL)
    return;

  snprintf(path, size, "%s%s", base, subId);

#if COR_FEATURE_AUTH
  authTrustNext();                                   // the broker's own request: the stream it notified is over
#endif
  corRestRunJson("DELETE", path, &tenantH, (tenant[0] != 0) ? 1 : 0, NULL, 0, runIgnore, NULL);
#if COR_FEATURE_AUTH
  authTrustClear();
#endif

  free(path);
}



// -----------------------------------------------------------------------------
//
// Done - what a tick does after it lets the mutex go: the counters of what was written, the
// subscriptions of the ended streams deleted
//
typedef struct Done
{
  char*         tenant;
  char*         subId;
  bool          csrSub;
  int           written;
  bool          remove;
  struct Done*  next;
} Done;

static Done* doneAdd(Done* list, SseEntry* eP, bool remove)
{
  Done* dP = (Done*) calloc(1, sizeof(Done));

  if (dP == NULL)
    return list;

  dP->tenant  = strdup(eP->tenant);
  dP->subId   = strdup(eP->subId);
  dP->csrSub  = eP->csrSub;
  dP->written = eP->written;
  dP->remove  = remove;
  dP->next    = list;

  if ((dP->tenant == NULL) || (dP->subId == NULL))
  {
    free(dP->tenant);
    free(dP->subId);
    free(dP);
    return list;
  }

  eP->written = 0;
  return dP;
}

static void doneRun(Done* list)
{
  uint64_t now = nowNs();

  while (list != NULL)
  {
    Done* dP = list;

    if (dP->written > 0)
    {
      Tenant* tP = tenantLookup(dP->tenant);

      if (tP != NULL)
        ldNotifyStreamSent((LdSubCache*) (dP->csrSub ? tP->regSubCacheP : tP->subCacheP), dP->subId, dP->csrSub, dP->written, now);
    }

    if (dP->remove == true)
    {
      subscriptionDelete(dP->tenant, dP->subId, dP->csrSub);
      COR_I("sse: subscription '%s' (tenant %s) deleted - its stream ended", dP->subId, (dP->tenant[0] != 0) ? dP->tenant : "default");
    }

    list = dP->next;
    free(dP->tenant);
    free(dP->subId);
    free(dP);
  }
}



// -----------------------------------------------------------------------------
//
// tick - once a second; see the top of the file. 'all': every stream ends (sseStop)
//
static void tick(bool all)
{
  Done*    list = NULL;
  uint64_t now  = nowMs();

  pthread_mutex_lock(&sseMutex);

  for (SseEntry* eP = entryList; eP != NULL; eP = eP->next)
  {
    if (eP->forgotten == true)
      continue;

    if (all == true)
      entryEnd(eP);
    else if ((eP->streamP != NULL) && (now - eP->lastWriteMs >= KEEP_ALIVE_MS))
      streamWrite(eP, ": keep-alive\n\n", 14);
    else if ((eP->streamP != NULL) && (corRestStreamWrite(eP->streamP, "", 0) == false))   // the client still there?
    {
      COR_I("sse: the client of '%s' left - its subscription goes", eP->subId);
      entryEnd(eP);
    }
    else if ((eP->opened == false) && (now - eP->madeMs >= OPEN_WAIT_MS))
      entryEnd(eP);

    if ((eP->written > 0) || (eP->ended == true))
      list = doneAdd(list, eP, eP->ended);

    if (eP->ended == true)
    {
      eP->forgotten = true;                           // out of the count now; freed by sseForget or below
      entries      -= 1;
    }
  }

  //
  // The forgotten entries go - but one whose open has not come yet is kept for it (it ends the stream);
  // the 30 s and sseStop cut that wait short
  //
  for (SseEntry** pP = &entryList; *pP != NULL; )
  {
    SseEntry* eP = *pP;

    if ((eP->forgotten == true) && ((eP->opened == true) || (all == true) || (now - eP->madeMs >= OPEN_WAIT_MS)))
    {
      *pP = eP->next;
      entryFree(eP);
    }
    else
      pP = &eP->next;
  }

  pthread_mutex_unlock(&sseMutex);

  doneRun(list);
}



// -----------------------------------------------------------------------------
//
// tickThread -
//
static void* tickThread(void* unused)
{
  (void) unused;

  pthread_mutex_lock(&sseMutex);

  while (stopping == false)
  {
    struct timespec deadline;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;
    pthread_cond_timedwait(&tickCond, &sseMutex, &deadline);

    if (stopping == true)
      break;

    pthread_mutex_unlock(&sseMutex);
    tick(false);
    pthread_mutex_lock(&sseMutex);
  }

  pthread_mutex_unlock(&sseMutex);
  return NULL;
}



// -----------------------------------------------------------------------------
//
// sseInit -
//
bool sseInit(int max)
{
  streamsMax = max;

  if (pthread_create(&tickTid, NULL, tickThread, NULL) != 0)
  {
    COR_E("sse: cannot start its thread - no event streams");
    return false;
  }

  tickUp = true;
  ldNotifyStreamSet(streamPut);

  return true;
}



// -----------------------------------------------------------------------------
//
// sseEndpointIs -
//
bool sseEndpointIs(CorNode* subP)
{
  if ((subP == NULL) || (subP->type != CorObject))
    return false;

  for (CorNode* nP = subP->value.head; nP != NULL; nP = nP->next)
  {
    if ((ldTermId(nP) != CorTermNotification) || (nP->type != CorObject))
      continue;

    for (CorNode* epP = nP->value.head; epP != NULL; epP = epP->next)
    {
      if ((ldTermId(epP) != CorTermEndpoint) || (epP->type != CorObject))
        continue;

      for (CorNode* uriP = epP->value.head; uriP != NULL; uriP = uriP->next)
      {
        if ((ldTermId(uriP) == CorTermUri) && (uriP->type == CorString))
          return ldNotifyIsEventStream(uriP->value.s);
      }
    }
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// acceptHas - the request's Accept names 'mediaRange' (case-insensitive)
//
static bool acceptHas(const char* mediaRange)
{
  const char* accept = corRest.in.accept;
  size_t      len    = strlen(mediaRange);

  if (accept == NULL)
    return false;

  for (const char* p = accept; *p != 0; p++)
  {
    if (strncasecmp(p, mediaRange, len) == 0)
      return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// streamAcceptable - a text/event-stream response is what the client takes: no Accept, or one with
// text/event-stream, text/* or */*
//
static bool streamAcceptable(void)
{
  if (corRest.in.accept == NULL)
    return true;

  return (acceptHas("text/event-stream") == true) || (acceptHas("text/*") == true) || (acceptHas("*/*") == true);
}



// -----------------------------------------------------------------------------
//
// sseStreamCreate -
//
bool sseStreamCreate(void)
{
  uint64_t ldOp = (corRest.serviceP != NULL) ? corRest.serviceP->ldOp : 0;

  return ((ldOp & (LdOpCreateSubscription | LdOpCreateCsourceSubscription)) != 0) && (acceptHas("text/event-stream") == true);
}



// -----------------------------------------------------------------------------
//
// sseCreateCheck -
//
bool sseCreateCheck(CorNode* subP, bool* streamP)
{
  *streamP = false;

  if (sseEndpointIs(subP) == false)
  {
    // Not on a stream: the Accept precondition the pre-service hook left for this route (sseStreamCreate)
    if (sseStreamCreate() == true)
      return ldAcceptPrecondition();

    return true;
  }

  if (streamAcceptable() == false)
  {
    ldError(406, LD_ERROR_INVALID_REQUEST, "Not Acceptable",
            "the response to a subscription on the endpoint 'urn:ngsi-ld:self' is its event stream: text/event-stream");
    return false;
  }

  if (corRestStreamPossible() == false)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Subscription", "an event stream is served over HTTP only");
    return false;
  }

  pthread_mutex_lock(&sseMutex);

  bool full = (stopping == true) || (entries + reserved >= streamsMax);

  if (full == false)
    reserved += 1;

  pthread_mutex_unlock(&sseMutex);

  if (full == true)
  {
    ldError(503, LD_ERROR_INTERNAL_ERROR, "Service Unavailable",
            stopping ? "the broker is stopping" : "the broker serves as many event streams as it may (--sseStreams)");
    return false;
  }

  *streamP = true;
  return true;
}



// -----------------------------------------------------------------------------
//
// sseCreateAbort -
//
void sseCreateAbort(void)
{
  pthread_mutex_lock(&sseMutex);
  reserved -= 1;
  pthread_mutex_unlock(&sseMutex);
}



// -----------------------------------------------------------------------------
//
// sseCreateRegister -
//
void sseCreateRegister(Tenant* tenantP, const char* subId, bool csrSub)
{
  SseEntry* eP = (SseEntry*) calloc(1, sizeof(SseEntry));

  if (eP != NULL)
  {
    eP->tenantP = tenantP;
    eP->tenant  = strdup(tenantApiName(tenantP));
    eP->subId   = strdup(subId);
    eP->csrSub  = csrSub;
    eP->madeMs  = nowMs();

    if ((eP->tenant == NULL) || (eP->subId == NULL))
    {
      entryFree(eP);
      eP = NULL;
    }
  }

  pthread_mutex_lock(&sseMutex);

  reserved -= 1;

  if (eP != NULL)
  {
    eP->id      = entryNext++;
    eP->next    = entryList;
    entryList   = eP;
    entries    += 1;
  }

  pthread_mutex_unlock(&sseMutex);

  if (eP == NULL)
    COR_E("sse: out of memory for the stream of '%s' - its notifications fail", subId);
}



// -----------------------------------------------------------------------------
//
// sseCreateRespond -
//
void sseCreateRespond(Tenant* tenantP, const char* subId, bool csrSub)
{
  pthread_mutex_lock(&sseMutex);
  SseEntry*          eP = entryFind(tenantP, subId, csrSub);
  unsigned long long id = (eP != NULL) ? eP->id : 0;
  pthread_mutex_unlock(&sseMutex);

  corRestOutHeaderAdd("Content-Type",      "text/event-stream");
  corRestOutHeaderAdd("Cache-Control",     "no-cache");
  corRestOutHeaderAdd("X-Accel-Buffering", "no");                // nginx (and APISIX) buffer a response otherwise

  if ((id == 0) || (corRestStreamResponse(streamOpened, (void*) (uintptr_t) id) == false))
  {
    pthread_mutex_lock(&sseMutex);
    if ((eP = entryById(id)) != NULL)
    {
      eP->opened = true;
      entryEnd(eP);                                   // no stream: the tick deletes the subscription
    }
    pthread_mutex_unlock(&sseMutex);
  }
}



// -----------------------------------------------------------------------------
//
// sseForget -
//
void sseForget(Tenant* tenantP, const char* subId, bool csrSub)
{
  pthread_mutex_lock(&sseMutex);

  SseEntry* eP = entryFind(tenantP, subId, csrSub);

  if (eP != NULL)
  {
    entryEnd(eP);
    eP->forgotten = true;
    entries      -= 1;

    if (eP->opened == true)
    {
      entryUnlink(eP);
      entryFree(eP);
    }
  }

  pthread_mutex_unlock(&sseMutex);
}



// -----------------------------------------------------------------------------
//
// leftoversOf - the subscriptions on an event stream in one cache, to the list
//
static Done* leftoversOf(Done* list, LdSubCache* cacheP, const char* tenant, bool csrSub)
{
  if (cacheP == NULL)
    return list;

  ldSubCacheRdLock(cacheP);

  for (LdSubCacheItem* iP = cacheP->itemList; iP != NULL; iP = iP->next)
  {
    if (ldNotifyIsEventStream(iP->endpointUri) == false)
      continue;

    Done* dP = (Done*) calloc(1, sizeof(Done));

    if (dP == NULL)
      break;

    dP->tenant = strdup(tenant);
    dP->subId  = strdup(iP->subId);
    dP->csrSub = csrSub;
    dP->remove = true;
    dP->next   = list;
    list       = dP;
  }

  ldSubCacheUnlock(cacheP);
  return list;
}



// -----------------------------------------------------------------------------
//
// pernotLeftoversOf - the periodic subscriptions on an event stream (their own cache), to the list
//
static Done* pernotLeftoversOf(Done* list, LdPernotCache* cacheP, const char* tenant)
{
  if (cacheP == NULL)
    return list;

  ldPernotCacheRdLock(cacheP);

  for (LdPernotItem* iP = cacheP->head; iP != NULL; iP = iP->next)
  {
    if (ldNotifyIsEventStream(iP->endpointUri) == false)
      continue;

    Done* dP = (Done*) calloc(1, sizeof(Done));

    if (dP == NULL)
      break;

    dP->tenant = strdup(tenant);
    dP->subId  = strdup(iP->subId);
    dP->remove = true;
    dP->next   = list;
    list       = dP;
  }

  ldPernotCacheUnlock(cacheP);
  return list;
}



// -----------------------------------------------------------------------------
//
// sseLeftoversDelete -
//
void sseLeftoversDelete(void)
{
  Done* list = NULL;

  for (Tenant* tP = &tenant0; tP != NULL; tP = (tP == &tenant0) ? __atomic_load_n(&tenantList, __ATOMIC_ACQUIRE) : tP->next)
  {
    list = leftoversOf(list, (LdSubCache*) tP->subCacheP,    tenantApiName(tP), false);
    list = leftoversOf(list, (LdSubCache*) tP->regSubCacheP, tenantApiName(tP), true);
    list = pernotLeftoversOf(list, (LdPernotCache*) tP->pernotCacheP, tenantApiName(tP));
  }

  for (Done* dP = list; dP != NULL; dP = dP->next)
  {
    if ((dP->tenant == NULL) || (dP->subId == NULL))
      dP->remove = false;
  }

  doneRun(list);
}



// -----------------------------------------------------------------------------
//
// sseStop -
//
void sseStop(void)
{
  pthread_mutex_lock(&sseMutex);
  stopping = true;
  pthread_cond_signal(&tickCond);
  pthread_mutex_unlock(&sseMutex);

  if (tickUp == true)
  {
    pthread_join(tickTid, NULL);
    tickUp = false;
  }

  tick(true);
}
