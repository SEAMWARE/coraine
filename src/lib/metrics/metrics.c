//
// FILE            metrics.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Prometheus metrics for coraine — see metrics.h.
//
#include <stdbool.h>                               // bool
#include <stddef.h>                                // NULL
#include <string.h>                                // strlen
#include <time.h>                                  // clock_gettime
#include <stdint.h>                                // uint64_t
#include <pthread.h>                               // pthread_mutex_t

#include "corAlloc/corAlloc.h"                     // corAlloc
#include "corTree/CorNode.h"                       // CorNode, CorArray
#include "corProm/corProm.h"                       // corProm*

#include "corRest/CorRestState.h"                    // corRest
#include "corRest/corRestInit.h"                     // corRestDispatchCounts
#include "corNgsild/LdOp.h"                         // LdOp*
#include "corNgsild/LdSubCache.h"                   // LdSubCache, LdSubCacheItem
#include "corNgsild/LdRegCache.h"                   // LdRegCache, LdRegCacheItem
#include "corNgsild/ldSubCache.h"                   // ldSubCacheRdLock, ldSubCacheUnlock
#include "corNgsild/ldRegCache.h"                   // ldRegCacheRdLock, ldRegCacheUnlock
#include "corNgsild/ldPernotCache.h"                // ldPernotCacheRdLock, ldPernotCacheUnlock
#include "corNgsild/ldEntityMap.h"                  // ldEntityMapStoreRdLock, ldEntityMapStoreUnlock, ldEntityMapBytesTotal
#include "corNgsild/LdPernotCache.h"                // LdPernotCache, LdPernotItem
#include "corNgsild/LdEntityMap.h"                  // LdEntityMapStore, LdEntityMap

#include "db/Tenant.h"                             // tenant0, tenantList

#include "metrics/metrics.h"                       // Own interface



// -----------------------------------------------------------------------------
//
// Per-LdOp request counters, indexed by LdOp bit position (0..63).
// LdOp is a bit-flag, so at most one bit set per service; the index is
// the trailing-zero count.
//
static CorPromMetric* reqCounterByOp[64];
static bool         initialized = false;

//
// Error counters
//
static CorPromMetric* errors4xx;
static CorPromMetric* errors5xx;

//
// Notification counters (entity-sub + CSR-sub, each with sent/failed).
//
static CorPromMetric* notifSent;
static CorPromMetric* notifFailed;
static CorPromMetric* csrNotifSent;
static CorPromMetric* csrNotifFailed;

//
// Cache-size gauges. Populated on-demand at render time by walking
// the per-tenant caches. Cheap: a few small linked-list counts per
// scrape, negligible relative to the HTTP roundtrip.
//
static CorPromMetric* gTenants;
static CorPromMetric* gSubCacheSize;
static CorPromMetric* gRegSubCacheSize;
static CorPromMetric* gRegCacheSize;
static CorPromMetric* gPernotCacheSize;
static CorPromMetric* gEntityMapStoreSize;
static CorPromMetric* gEntityMapBytes;

//
// Where requests were processed - on the I/O thread that read them (inline) or handed to a worker.
// corRest counts; these follow it at every scrape (dispatchCounts).
//
static CorPromMetric*  dispatchInline;
static CorPromMetric*  dispatchWorker;

//
// The memory budget (src/app/coraine/memoryBudget.h): what it is, where the broker stands, and how many
// requests it turned away. Read at every scrape through memoryValuesFunc, which the broker sets - the
// budget lives in the app, not in this lib.
//
static CorPromMetric*  memBudget;
static CorPromMetric*  memUsed;
static CorPromMetric*  memResident;
static CorPromMetric*  memRefused;
static MetricsMemoryValuesFunc memoryValuesFunc = NULL;

//
// Distop forwarding — counters + latency histogram.
//
static CorPromMetric* distopForwarded;
static CorPromMetric* distopForwardFailed;
static CorPromMetric* distopLatency;

// End-to-end request latency — observed in the post-response hook.
// Buckets cover intra-DC HTTP roundtrips typical for entity ops
// (fast path sub-ms) and outliers out to a few seconds for distops
// and large batches.
static CorPromMetric* requestLatency;
static double       requestLatencyBuckets[] = { 0.0005, 0.001, 0.002, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5 };

// Response body bytes, observed in the same post-response hook. The one
// HTTP-level number the broker did not have: request counts, error counts and
// latency were all here already, bytes were not. corRest renders the body and
// leaves its size in corRest.out.payloadSize, which is still valid when the
// hook runs (the hook fires before corRestStateRelease).
static CorPromMetric* responseBodyBytes;

// Buckets in seconds. Tuned for typical intra-DC HTTP roundtrips with
// tail coverage out to 5s to catch slow CPs. The +Inf bucket is added
// by corProm automatically.
static double distopLatencyBuckets[] = { 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0 };

// Batch op item count — observed in metricsPreService when the op is
// one of the six /entityOperations/* batch ops and the payload tree
// is a JSON array.
static CorPromMetric* batchItemCount;
static double       batchItemCountBuckets[] = { 1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500 };

// Mask of LdOp bits for the six /entityOperations/* batch ops
// (bits 12..17: create, upsert, update, delete, merge, query).
#define LD_OPS_BATCH_MASK ( (1ULL << 12) | (1ULL << 13) | (1ULL << 14) | \
                            (1ULL << 15) | (1ULL << 16) | (1ULL << 17) )



// -----------------------------------------------------------------------------
//
// The name and help-text table for each op bit. Kept as a simple array
// so the init loop stays readable. Name-bit mismatch is a compile-time
// bug if/when new LdOps are added — bump the table.
//
typedef struct OpMeta
{
  int         bit;
  const char* name;
  const char* help;
} OpMeta;

static const OpMeta opTable[] =
{
  //                                     bit, metric name,                                       help
  { 0,  "ngsild_entity_create_total",            "Entity creation requests (POST /entities)" },
  { 1,  "ngsild_entity_update_total",            "Entity legacy-update requests" },
  { 2,  "ngsild_entity_attrs_append_total",      "Attribute append requests (POST /entities/*/attrs)" },
  { 3,  "ngsild_entity_attrs_update_total",      "Attribute update requests (PATCH /entities/*/attrs*)" },
  { 4,  "ngsild_entity_merge_total",             "Entity merge requests (PATCH /entities/*)" },
  { 5,  "ngsild_entity_replace_total",           "Entity replace requests (PUT /entities/*)" },
  { 6,  "ngsild_entity_delete_total",            "Entity delete requests (DELETE /entities/*)" },
  { 7,  "ngsild_entity_attr_delete_total",       "Attribute delete requests" },
  { 8,  "ngsild_entity_attr_replace_total",      "Attribute replace requests (PUT /entities/*/attrs/*)" },
  { 9,  "ngsild_entity_purge_total",             "Entity purge requests (DELETE /entities)" },
  { 10, "ngsild_entity_retrieve_total",          "Entity retrieve requests (GET /entities/*)" },
  { 11, "ngsild_entity_query_total",             "Entity query requests (GET /entities)" },
  { 12, "ngsild_batch_create_total",             "Batch create requests" },
  { 13, "ngsild_batch_upsert_total",             "Batch upsert requests" },
  { 14, "ngsild_batch_update_total",             "Batch update requests" },
  { 15, "ngsild_batch_delete_total",             "Batch delete requests" },
  { 16, "ngsild_batch_merge_total",              "Batch merge requests" },
  { 17, "ngsild_batch_query_total",              "Batch query requests" },
  { 18, "ngsild_entity_types_get_total",         "Entity types listing" },
  { 19, "ngsild_entity_type_details_total",      "Entity type details" },
  { 20, "ngsild_entity_type_info_total",         "Entity type info" },
  { 21, "ngsild_attr_types_get_total",           "Attribute types listing" },
  { 22, "ngsild_attr_type_details_total",        "Attribute type details" },
  { 23, "ngsild_attr_type_info_total",           "Attribute type info" },
  { 24, "ngsild_subscription_create_total",      "Subscription create" },
  { 25, "ngsild_subscription_update_total",      "Subscription update" },
  { 26, "ngsild_subscription_retrieve_total",    "Subscription retrieve" },
  { 27, "ngsild_subscription_query_total",       "Subscription query" },
  { 28, "ngsild_subscription_delete_total",      "Subscription delete" },
  { 29, "ngsild_registration_create_total",      "CSource registration create" },
  { 30, "ngsild_registration_update_total",      "CSource registration update" },
  { 31, "ngsild_registration_retrieve_total",    "CSource registration retrieve" },
  { 32, "ngsild_registration_query_total",       "CSource registration query" },
  { 33, "ngsild_registration_delete_total",      "CSource registration delete" },
  { 34, "ngsild_csr_sub_create_total",           "CSR subscription create (§ 5.11)" },
  { 35, "ngsild_csr_sub_update_total",           "CSR subscription update (§ 5.11)" },
  { 36, "ngsild_csr_sub_retrieve_total",         "CSR subscription retrieve (§ 5.11)" },
  { 37, "ngsild_csr_sub_query_total",            "CSR subscription query (§ 5.11)" },
  { 38, "ngsild_csr_sub_delete_total",           "CSR subscription delete (§ 5.11)" }
};

static const int opTableSize = sizeof(opTable) / sizeof(opTable[0]);



// -----------------------------------------------------------------------------
//
// metricsInit -
//
bool metricsInit(void)
{
  if (initialized)
    return true;

  for (int i = 0; i < opTableSize; i++)
    reqCounterByOp[opTable[i].bit] = corPromCounterCreate(opTable[i].name, opTable[i].help);

  errors4xx = corPromCounterCreate("ngsild_errors_4xx_total",
                                 "Responses with 4xx status code (client-side error)");
  errors5xx = corPromCounterCreate("ngsild_errors_5xx_total",
                                 "Responses with 5xx status code (server-side error)");

  notifSent      = corPromCounterCreate("ngsild_notifications_sent_total",
                                      "Entity-subscription notifications POSTed (2xx reply)");
  notifFailed    = corPromCounterCreate("ngsild_notifications_failed_total",
                                      "Entity-subscription notifications that failed (non-2xx or no reply)");
  csrNotifSent   = corPromCounterCreate("ngsild_csource_notifications_sent_total",
                                      "CSR-subscription notifications POSTed (§ 5.11)");
  csrNotifFailed = corPromCounterCreate("ngsild_csource_notifications_failed_total",
                                      "CSR-subscription notifications that failed");

  gTenants            = corPromGaugeCreate("ngsild_tenants_total",
                                         "Number of tenants (including default)");
  gSubCacheSize       = corPromGaugeCreate("ngsild_subscription_cache_size",
                                         "Entity-subscriptions cached (sum across tenants)");
  gRegSubCacheSize    = corPromGaugeCreate("ngsild_csource_subscription_cache_size",
                                         "CSR-subscriptions cached (sum across tenants)");
  gRegCacheSize       = corPromGaugeCreate("ngsild_csource_registration_cache_size",
                                         "Context Source registrations cached (sum across tenants)");
  gPernotCacheSize    = corPromGaugeCreate("ngsild_pernot_cache_size",
                                         "Periodic-notification subscriptions cached (sum across tenants)");
  gEntityMapStoreSize = corPromGaugeCreate("ngsild_entity_map_store_size",
                                         "EntityMap store entries (sum across tenants)");
  gEntityMapBytes     = corPromGaugeCreate("ngsild_entity_map_bytes",
                                         "Memory the EntityMaps hold, automatic and requested (an estimate; the cap is --entityMapMemory)");

  dispatchInline      = corPromCounterCreate("ngsild_requests_inline_total",
                                           "Requests processed on the I/O thread that read them (see --noInline)");
  dispatchWorker      = corPromCounterCreate("ngsild_requests_worker_total",
                                           "Requests handed from their I/O thread to a worker thread");

  memBudget           = corPromGaugeCreate("ngsild_memory_budget_bytes",
                                         "Memory budget (--memoryLimit, else 85% of the container's limit); 0 = none");
  memUsed             = corPromGaugeCreate("ngsild_memory_used_bytes",
                                         "Memory counted against the budget: anonymous + shared (RssAnon + RssShmem)");
  memResident         = corPromGaugeCreate("ngsild_memory_resident_bytes",
                                         "The broker's resident set, file-backed pages included");
  memRefused          = corPromCounterCreate("ngsild_requests_refused_memory_total",
                                           "Requests refused (503) for being over the memory budget");

  distopForwarded     = corPromCounterCreate("ngsild_distop_forwarded_total",
                                           "Distributed-op forward attempts (every outbound request)");
  distopForwardFailed = corPromCounterCreate("ngsild_distop_forward_failed_total",
                                           "Distributed-op forwards that failed (transport error or non-2xx)");
  distopLatency       = corPromHistogramCreate("ngsild_distop_forward_latency_seconds",
                                             "Distributed-op forward round-trip latency (seconds)",
                                             distopLatencyBuckets,
                                             sizeof(distopLatencyBuckets) / sizeof(distopLatencyBuckets[0]));

  responseBodyBytes   = corPromCounterCreate("ngsild_response_body_bytes_total",
                                           "Response body bytes rendered (sum over all requests)");

  requestLatency      = corPromHistogramCreate("ngsild_request_latency_seconds",
                                             "End-to-end request latency — service-routine processing time (seconds)",
                                             requestLatencyBuckets,
                                             sizeof(requestLatencyBuckets) / sizeof(requestLatencyBuckets[0]));

  batchItemCount      = corPromHistogramCreate("ngsild_batch_item_count",
                                             "Number of items per /entityOperations batch request",
                                             batchItemCountBuckets,
                                             sizeof(batchItemCountBuckets) / sizeof(batchItemCountBuckets[0]));

  initialized = true;
  return true;
}



// -----------------------------------------------------------------------------
//
// tenantCounts - walk each tenant's caches once per scrape and update gauges
//
static void tenantCounts(void)
{
  int tenants  = 0;
  int subs     = 0;
  int regSubs  = 0;
  int regs     = 0;
  int pernots  = 0;
  int maps     = 0;

  // tenant0 always exists
  for (Tenant* tP = &tenant0;
       tP != NULL;
       tP = (tP == &tenant0) ? tenantList : tP->next)
  {
    tenants++;

    LdSubCache*       sc   = (LdSubCache*)       tP->subCacheP;
    LdSubCache*       rsc  = (LdSubCache*)       tP->regSubCacheP;
    LdRegCache*       rc   = (LdRegCache*)       tP->regCacheP;
    LdPernotCache*    pc   = (LdPernotCache*)    tP->pernotCacheP;
    LdEntityMapStore* ems  = (LdEntityMapStore*) tP->entityMapStoreP;

    //
    // Each count under its cache's rdlock - they followed `next` with no lock, while requests
    // added and freed items. One lock at a time, so no lock order to keep.
    //
    if (sc != NULL)  { ldSubCacheRdLock(sc);  for (LdSubCacheItem* i = sc->itemList;  i != NULL; i = i->next) subs++;    ldSubCacheUnlock(sc);  }
    if (rsc != NULL) { ldSubCacheRdLock(rsc); for (LdSubCacheItem* i = rsc->itemList; i != NULL; i = i->next) regSubs++; ldSubCacheUnlock(rsc); }
    if (rc != NULL)  { ldRegCacheRdLock(rc);  for (LdRegCacheItem* i = rc->itemList;  i != NULL; i = i->next) regs++;    ldRegCacheUnlock(rc);  }
    if (pc != NULL)  { ldPernotCacheRdLock(pc); for (LdPernotItem* i = pc->head; i != NULL; i = i->next) pernots++; ldPernotCacheUnlock(pc); }
    if (ems != NULL) { ldEntityMapStoreRdLock(ems); for (LdEntityMap* i = ems->head; i != NULL; i = i->next) maps++; ldEntityMapStoreUnlock(ems); }
  }

  corPromGaugeSet(gTenants,          (double) tenants);
  corPromGaugeSet(gSubCacheSize,     (double) subs);
  corPromGaugeSet(gRegSubCacheSize,  (double) regSubs);
  corPromGaugeSet(gRegCacheSize,     (double) regs);
  corPromGaugeSet(gPernotCacheSize,  (double) pernots);
  corPromGaugeSet(gEntityMapStoreSize, (double) maps);
  corPromGaugeSet(gEntityMapBytes,     (double) ldEntityMapBytesTotal());
}



// -----------------------------------------------------------------------------
//
// metricsPreService -
//
bool metricsPreService(void)
{
  if (corRest.serviceP == NULL)
    return true;

  uint64_t op = corRest.serviceP->ldOp;
  if (op == 0)
    return true;

  int bit = __builtin_ctzll(op);
  if (bit >= 0 && bit < 64 && reqCounterByOp[bit] != NULL)
    corPromCounterInc(reqCounterByOp[bit]);

  // For batch ops, observe the array length so we can size payloads
  // in dashboards.
  if ((op & LD_OPS_BATCH_MASK) != 0 &&
      corRest.in.requestTree != NULL &&
      corRest.in.requestTree->type == CorArray)
  {
    int n = 0;
    for (CorNode* c = corRest.in.requestTree->value.head; c != NULL; c = c->next)
      n++;
    corPromHistogramObserve(batchItemCount, (double) n);
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// metricsPostResponse -
//
void metricsPostResponse(void)
{
  int sc = corRest.out.httpStatusCode;
  if (sc >= 500 && sc < 600)
    corPromCounterInc(errors5xx);
  else if (sc >= 400 && sc < 500)
    corPromCounterInc(errors4xx);

  if (corRest.out.payloadSize > 0)
    corPromCounterAdd(responseBodyBytes, corRest.out.payloadSize);

  // End-to-end request latency. requestStartTimeMono is CLOCK_MONOTONIC
  // nanoseconds (set by corRest on request entry — the CorRestState.h
  // comment that says "microseconds" is wrong; see corRestInit.c:384).
  // Guard the "never observed" case where the start time is 0.
  if (corRest.requestStartTimeMono != 0)
  {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t nowNs   = (uint64_t) now.tv_sec * 1000000000ULL + (uint64_t) now.tv_nsec;
    double   latency = (double) (nowNs - corRest.requestStartTimeMono) / 1e9;
    corPromHistogramObserve(requestLatency, latency);
  }
}



// -----------------------------------------------------------------------------
//
// metricsNotificationSent -
//
void metricsNotificationSent(bool success)
{
  if (success) corPromCounterInc(notifSent);
  else         corPromCounterInc(notifFailed);
}



// -----------------------------------------------------------------------------
//
// metricsCsrNotificationSent -
//
void metricsCsrNotificationSent(bool success)
{
  if (success) corPromCounterInc(csrNotifSent);
  else         corPromCounterInc(csrNotifFailed);
}



// -----------------------------------------------------------------------------
//
// metricsDistopForward -
//
void metricsDistopForward(double latencySec, bool success)
{
  corPromCounterInc(distopForwarded);
  if (!success)
    corPromCounterInc(distopForwardFailed);
  corPromHistogramObserve(distopLatency, latencySec);
}



// -----------------------------------------------------------------------------
//
// dispatchCounts - bring the dispatch counters up to corRest's
//
// A counter only goes up, so what is added is what happened since the last scrape; the mutex keeps
// two scrapes at once from adding the same requests twice.
//
static void dispatchCounts(void)
{
  static pthread_mutex_t mtx          = PTHREAD_MUTEX_INITIALIZER;
  static uint64_t        inlineSeen   = 0;
  static uint64_t        workerSeen   = 0;
  uint64_t               inlineN;
  uint64_t               workerN;

  pthread_mutex_lock(&mtx);
  corRestDispatchCounts(&inlineN, &workerN);
  corPromCounterAdd(dispatchInline, (int64_t) (inlineN - inlineSeen));
  corPromCounterAdd(dispatchWorker, (int64_t) (workerN - workerSeen));
  inlineSeen = inlineN;
  workerSeen = workerN;
  pthread_mutex_unlock(&mtx);
}



// -----------------------------------------------------------------------------
//
// metricsMemoryValuesSet - where the memory figures come from
//
void metricsMemoryValuesSet(MetricsMemoryValuesFunc fn)
{
  memoryValuesFunc = fn;
}



// -----------------------------------------------------------------------------
//
// memoryCounts - the memory gauges, and the refused counter brought up to date (as dispatchCounts)
//
static void memoryCounts(void)
{
  static pthread_mutex_t mtx         = PTHREAD_MUTEX_INITIALIZER;
  static uint64_t        refusedSeen = 0;
  uint64_t               budget;
  uint64_t               used;
  uint64_t               resident;
  uint64_t               refused;

  if (memoryValuesFunc == NULL)
    return;

  memoryValuesFunc(&budget, &used, &resident, &refused);
  corPromGaugeSet(memBudget,   (double) budget);
  corPromGaugeSet(memUsed,     (double) used);
  corPromGaugeSet(memResident, (double) resident);

  pthread_mutex_lock(&mtx);
  corPromCounterAdd(memRefused, (int64_t) (refused - refusedSeen));
  refusedSeen = refused;
  pthread_mutex_unlock(&mtx);
}



// -----------------------------------------------------------------------------
//
// metricsRender -
//
bool metricsRender(void)
{
  tenantCounts();
  dispatchCounts();
  memoryCounts();

  //
  // +1 for the terminating NUL. corPromRenderSize() reports the payload EXCLUDING
  // it, and corPromRender is built on snprintf, which always writes one - so a
  // buffer of exactly bufSize loses its final byte, and that byte is the newline
  // ending the last metric. The result parses as binary rather than as the
  // Prometheus text format, and corPromRender cannot report it: on an exact fit it
  // returns the same length it would have returned had everything fitted.
  //
  int   bufSize = corPromRenderSize();
  char* buf     = (char*) corAlloc(&corRest.kalloc, bufSize + 1);

  if (buf == NULL)
  {
    corRest.out.httpStatusCode = 500;
    return true;
  }

  //
  // bufSize + 1 here too - this is the buffer LENGTH, which is what snprintf
  // sizes against. Widening the allocation without widening this changes nothing.
  //
  int rendered = corPromRender(buf, bufSize + 1);
  if (rendered < 0)
  {
    corRest.out.httpStatusCode = 500;
    return true;
  }

  corRest.out.payload        = buf;
  corRest.out.payloadSize    = rendered;
  corRest.out.contentType    = "text/plain; version=0.0.4";
  corRest.out.httpStatusCode = 200;
  return true;
}
