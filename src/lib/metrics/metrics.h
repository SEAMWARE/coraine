#ifndef METRICS_METRICS_H_
#define METRICS_METRICS_H_

//
// FILE            metrics.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Prometheus metrics facade for swBroker. All counters/gauges live
// in a single translation unit (metrics.c). Hot-path mutators are
// kprom atomic ops — no allocation, no locks.
//
// Wiring:
//   - metricsInit()           at startup, after broker args parsed.
//   - metricsPreService()     from swRest pre-service hook wrapper.
//   - metricsPostResponse()   from swRest post-response hook wrapper.
//   - metricsNotificationSent() / ...CsrSent() from notifier code.
//   - metricsRender()         GET /admin/metrics handler.
//
#include <stdbool.h>                               // bool



// -----------------------------------------------------------------------------
//
// metricsInit - create all KpromMetric objects
//
// Called once at broker startup, after kargs parse, before the
// REST server is spun up. Idempotent — a second call is a no-op.
//
extern bool metricsInit(void);



// -----------------------------------------------------------------------------
//
// metricsPreService - pre-service hook: bump the per-LdOp counter
//
// Returns true so it composes cleanly with other pre-service hooks.
//
extern bool metricsPreService(void);



// -----------------------------------------------------------------------------
//
// metricsPostResponse - post-response hook: bump 4xx/5xx on error
//
extern void metricsPostResponse(void);



// -----------------------------------------------------------------------------
//
// metricsNotificationSent - bump entity-sub notification counter
//
//   success == true  -> ngsild_notifications_sent_total
//   success == false -> ngsild_notifications_failed_total
//
extern void metricsNotificationSent(bool success);



// -----------------------------------------------------------------------------
//
// metricsCsrNotificationSent - bump CSR-sub notification counter
//
//   success == true  -> ngsild_csource_notifications_sent_total
//   success == false -> ngsild_csource_notifications_failed_total
//
extern void metricsCsrNotificationSent(bool success);



// -----------------------------------------------------------------------------
//
// metricsRender - Prometheus text-format output for /admin/metrics
//
// Service-routine style: uses swRest.out.payload etc. Sets Content-Type
// to "text/plain; version=0.0.4" per Prometheus exposition spec.
//
extern bool metricsRender(void);

#endif  // METRICS_METRICS_H_
