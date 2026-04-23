//
// FILE            metrics.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Prometheus metrics for swBroker — see metrics.h.
//
#include <stdbool.h>                               // bool
#include <stddef.h>                                // NULL
#include <string.h>                                // strlen

#include "kalloc/kaAlloc.h"                        // kaAlloc
#include "kprom/kprom.h"                           // kprom*

#include "swRest/SwRestState.h"                    // swRest
#include "swNgsild/LdOp.h"                         // LdOp*
#include "swNgsild/LdSubCache.h"                   // LdSubCache, LdSubCacheItem
#include "swNgsild/LdRegCache.h"                   // LdRegCache, LdRegCacheItem
#include "swNgsild/LdPernotCache.h"                // LdPernotCache, LdPernotItem
#include "swNgsild/LdEntityMap.h"                  // LdEntityMapStore, LdEntityMap

#include "db/Tenant.h"                             // tenant0, tenantList

#include "metrics/metrics.h"                       // Own interface



// -----------------------------------------------------------------------------
//
// Per-LdOp request counters, indexed by LdOp bit position (0..63).
// LdOp is a bit-flag, so at most one bit set per service; the index is
// the trailing-zero count.
//
static KpromMetric* reqCounterByOp[64];
static bool         initialized = false;

//
// Error counters
//
static KpromMetric* errors4xx;
static KpromMetric* errors5xx;

//
// Notification counters (entity-sub + CSR-sub, each with sent/failed).
//
static KpromMetric* notifSent;
static KpromMetric* notifFailed;
static KpromMetric* csrNotifSent;
static KpromMetric* csrNotifFailed;

//
// Cache-size gauges. Populated on-demand at render time by walking
// the per-tenant caches. Cheap: a few small linked-list counts per
// scrape, negligible relative to the HTTP roundtrip.
//
static KpromMetric* gTenants;
static KpromMetric* gSubCacheSize;
static KpromMetric* gRegSubCacheSize;
static KpromMetric* gRegCacheSize;
static KpromMetric* gPernotCacheSize;
static KpromMetric* gEntityMapStoreSize;

//
// Distop forwarding — counters + latency histogram.
//
static KpromMetric* distopForwarded;
static KpromMetric* distopForwardFailed;
static KpromMetric* distopLatency;

// Buckets in seconds. Tuned for typical intra-DC HTTP roundtrips with
// tail coverage out to 5s to catch slow CPs. The +Inf bucket is added
// by kprom automatically.
static double distopLatencyBuckets[] = { 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0 };



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
    reqCounterByOp[opTable[i].bit] = kpromCounterCreate(opTable[i].name, opTable[i].help);

  errors4xx = kpromCounterCreate("ngsild_errors_4xx_total",
                                 "Responses with 4xx status code (client-side error)");
  errors5xx = kpromCounterCreate("ngsild_errors_5xx_total",
                                 "Responses with 5xx status code (server-side error)");

  notifSent      = kpromCounterCreate("ngsild_notifications_sent_total",
                                      "Entity-subscription notifications POSTed (2xx reply)");
  notifFailed    = kpromCounterCreate("ngsild_notifications_failed_total",
                                      "Entity-subscription notifications that failed (non-2xx or no reply)");
  csrNotifSent   = kpromCounterCreate("ngsild_csource_notifications_sent_total",
                                      "CSR-subscription notifications POSTed (§ 5.11)");
  csrNotifFailed = kpromCounterCreate("ngsild_csource_notifications_failed_total",
                                      "CSR-subscription notifications that failed");

  gTenants            = kpromGaugeCreate("ngsild_tenants_total",
                                         "Number of tenants (including default)");
  gSubCacheSize       = kpromGaugeCreate("ngsild_subscription_cache_size",
                                         "Entity-subscriptions cached (sum across tenants)");
  gRegSubCacheSize    = kpromGaugeCreate("ngsild_csource_subscription_cache_size",
                                         "CSR-subscriptions cached (sum across tenants)");
  gRegCacheSize       = kpromGaugeCreate("ngsild_csource_registration_cache_size",
                                         "Context Source registrations cached (sum across tenants)");
  gPernotCacheSize    = kpromGaugeCreate("ngsild_pernot_cache_size",
                                         "Periodic-notification subscriptions cached (sum across tenants)");
  gEntityMapStoreSize = kpromGaugeCreate("ngsild_entity_map_store_size",
                                         "EntityMap store entries (sum across tenants)");

  distopForwarded     = kpromCounterCreate("ngsild_distop_forwarded_total",
                                           "Distributed-op forward attempts (every outbound request)");
  distopForwardFailed = kpromCounterCreate("ngsild_distop_forward_failed_total",
                                           "Distributed-op forwards that failed (transport error or non-2xx)");
  distopLatency       = kpromHistogramCreate("ngsild_distop_forward_latency_seconds",
                                             "Distributed-op forward round-trip latency (seconds)",
                                             distopLatencyBuckets,
                                             sizeof(distopLatencyBuckets) / sizeof(distopLatencyBuckets[0]));

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

    if (sc != NULL)  for (LdSubCacheItem* i = sc->itemList;  i != NULL; i = i->next) subs++;
    if (rsc != NULL) for (LdSubCacheItem* i = rsc->itemList; i != NULL; i = i->next) regSubs++;
    if (rc != NULL)  for (LdRegCacheItem* i = rc->itemList;  i != NULL; i = i->next) regs++;
    if (pc != NULL)  for (LdPernotItem*   i = pc->head;      i != NULL; i = i->next) pernots++;
    if (ems != NULL) for (LdEntityMap*    i = ems->head;     i != NULL; i = i->next) maps++;
  }

  kpromGaugeSet(gTenants,            (double) tenants);
  kpromGaugeSet(gSubCacheSize,       (double) subs);
  kpromGaugeSet(gRegSubCacheSize,    (double) regSubs);
  kpromGaugeSet(gRegCacheSize,       (double) regs);
  kpromGaugeSet(gPernotCacheSize,    (double) pernots);
  kpromGaugeSet(gEntityMapStoreSize, (double) maps);
}



// -----------------------------------------------------------------------------
//
// metricsPreService -
//
bool metricsPreService(void)
{
  if (swRest.serviceP == NULL)
    return true;

  uint64_t op = swRest.serviceP->ldOp;
  if (op == 0)
    return true;

  int bit = __builtin_ctzll(op);
  if (bit >= 0 && bit < 64 && reqCounterByOp[bit] != NULL)
    kpromCounterInc(reqCounterByOp[bit]);

  return true;
}



// -----------------------------------------------------------------------------
//
// metricsPostResponse -
//
void metricsPostResponse(void)
{
  int sc = swRest.out.httpStatusCode;
  if (sc >= 500 && sc < 600)
    kpromCounterInc(errors5xx);
  else if (sc >= 400 && sc < 500)
    kpromCounterInc(errors4xx);
}



// -----------------------------------------------------------------------------
//
// metricsNotificationSent -
//
void metricsNotificationSent(bool success)
{
  if (success) kpromCounterInc(notifSent);
  else         kpromCounterInc(notifFailed);
}



// -----------------------------------------------------------------------------
//
// metricsCsrNotificationSent -
//
void metricsCsrNotificationSent(bool success)
{
  if (success) kpromCounterInc(csrNotifSent);
  else         kpromCounterInc(csrNotifFailed);
}



// -----------------------------------------------------------------------------
//
// metricsDistopForward -
//
void metricsDistopForward(double latencySec, bool success)
{
  kpromCounterInc(distopForwarded);
  if (!success)
    kpromCounterInc(distopForwardFailed);
  kpromHistogramObserve(distopLatency, latencySec);
}



// -----------------------------------------------------------------------------
//
// metricsRender -
//
bool metricsRender(void)
{
  tenantCounts();

  int   bufSize = kpromRenderSize();
  char* buf     = (char*) kaAlloc(&swRest.kalloc, bufSize);

  if (buf == NULL)
  {
    swRest.out.httpStatusCode = 500;
    return true;
  }

  int rendered = kpromRender(buf, bufSize);
  if (rendered < 0)
  {
    swRest.out.httpStatusCode = 500;
    return true;
  }

  swRest.out.payload        = buf;
  swRest.out.payloadSize    = rendered;
  swRest.out.contentType    = "text/plain; version=0.0.4";
  swRest.out.httpStatusCode = 200;
  return true;
}
