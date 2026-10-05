#ifndef SRC_LIB_BRIDGE_BRIDGESERVICE_H_
#define SRC_LIB_BRIDGE_BRIDGESERVICE_H_

//
// FILE            bridgeService.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The broker as a Service Executor (Service Execution, doc/service-execution.md): a Service
// Registration whose endpoint has a scheme a loaded bridge executes (BridgeDriver.serviceSchemes,
// ABI 11) is executed by that bridge - serviceExecute, serviceCancel - and the bridge reports through
// BridgeBroker.serviceUpdateIn.
//
// A synchronous service: the request's thread waits for the outcome. An asynchronous one: each report
// is applied on the plugin's thread, as the execution's tenant, through the function Service Execution
// sets (bridgeServiceApplySet) - this library knows no executions.
//
#include <stdbool.h>                                  // bool



// -----------------------------------------------------------------------------
//
// BridgeServiceResult - a synchronous execution's outcome
//
typedef struct BridgeServiceResult
{
  char  status[16];                                   // "completed", "failed"; "" - no outcome in time
  char* outputJson;                                   // malloc'd, or NULL - bridgeServiceResultRelease
  char* errorJson;                                    // malloc'd, or NULL
} BridgeServiceResult;



// -----------------------------------------------------------------------------
//
// BridgeServiceApplyFn - an asynchronous execution's report, applied (on the plugin's thread, bound)
//
typedef void (*BridgeServiceApplyFn)(const char* executionId, const char* status, const char* progressJson, const char* outputJson, const char* errorJson);



// -----------------------------------------------------------------------------
//
// bridgeServiceHas - does a loaded bridge execute services of this URI's scheme?
//
extern bool bridgeServiceHas(const char* url);



// -----------------------------------------------------------------------------
//
// bridgeServiceExecute - hand an execution to its bridge
//
// waitMs > 0: a synchronous service - wait up to waitMs for its outcome, into *resultP (status ""
// when none came: the execution is then forgotten, and cancelled if the bridge can). waitMs 0: an
// asynchronous one - its reports are applied as they come.
//
// @return a BRIDGE_* code: BRIDGE_OK, BRIDGE_NOT_FOUND (no bridge, or no such service),
//         BRIDGE_BAD_INPUT, BRIDGE_ERR
//
extern int bridgeServiceExecute(const char* url, const char* executionId, const char* tenantName, const char* inputJson, int waitMs, BridgeServiceResult* resultP);



// -----------------------------------------------------------------------------
//
// bridgeServiceResultRelease -
//
extern void bridgeServiceResultRelease(BridgeServiceResult* resultP);



// -----------------------------------------------------------------------------
//
// bridgeServiceCancel - BRIDGE_OK (cancelled), BRIDGE_UNSUPPORTED (cannot be pre-empted), BRIDGE_NOT_FOUND, BRIDGE_ERR
//
extern int bridgeServiceCancel(const char* url, const char* executionId);



// -----------------------------------------------------------------------------
//
// bridgeServiceApplySet - who applies an asynchronous execution's reports (Service Execution, at startup)
//
extern void bridgeServiceApplySet(BridgeServiceApplyFn applyFn);



// -----------------------------------------------------------------------------
//
// bridgeServiceUpdateIn - BridgeBroker.serviceUpdateIn, ABI 11
//
extern int bridgeServiceUpdateIn(const char* bridgeName, const char* executionId, const char* status, const char* progressJson, const char* outputJson, const char* errorJson);

#endif  // SRC_LIB_BRIDGE_BRIDGESERVICE_H_
