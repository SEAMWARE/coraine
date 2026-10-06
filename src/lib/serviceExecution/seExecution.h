#ifndef SRC_LIB_SERVICEEXECUTION_SEEXECUTION_H_
#define SRC_LIB_SERVICEEXECUTION_SEEXECUTION_H_

//
// FILE            seExecution.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Service Executions (doc/service-execution.md): one invocation of a registered service on one entity,
// a document of the "serviceExecutions" collection, with a lifecycle
//
//   pending ──▶ executing ──▶ completed
//      │            │    └──▶ failed       (executionError, a ProblemDetails)
//      └────────────┴───────▶ cancelled
//
// and its notifications: every change is notified to the execution's own `notification` endpoint
// (else the registration's), as an NGSI-LD Notification - http(s), or any transport (a WebSocket).
//
// Stored besides what is rendered, each member's name starting '_' (never rendered):
//   _registration   the Service Registration's id
//   _endpoint       the executor's URI, _mode "synchronous" / "asynchronous", _timeoutNs
//   _startedNs, _endedNs   nanoseconds - the time-out and the retention
//   _context        the @context URL of the request that created it - its notifications are compacted with it
//
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // int64_t

#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "db/Tenant.h"                                // Tenant



// -----------------------------------------------------------------------------
//
// SeOrigin - which operation started the execution: their answers differ
//
typedef enum SeOrigin
{
  SeInvoke,                                           // POST /entities/{id}/services/{name}
  SeCreate                                            // POST /services
} SeOrigin;



// -----------------------------------------------------------------------------
//
// SeForward - what an executor answered a hand-off (or a cancel)
//
typedef struct SeForward
{
  bool      accepted;                                 // a 2xx (HTTP), BRIDGE_OK (a bridge)
  int       clientStatus;                             // for the client when not accepted: 400, the executor's 4xx, 409, 502, 503, 504
  CorNode*  outputP;                                  // a synchronous result
  CorNode*  errorP;                                   // a ProblemDetails when not accepted
} SeForward;



// -----------------------------------------------------------------------------
//
// seExecutionRetentionNs - how long a finished execution is kept (--serviceExecutionRetention)
//
extern int64_t seExecutionRetentionNs;



// -----------------------------------------------------------------------------
//
// seExecute - start an execution of 'serviceName' (expanded) on the entity, and answer the request
//
// The entity must exist and a Service Registration must offer the service for it; the input must
// satisfy the service's inputSchema. A synchronous service is waited for; an asynchronous one is handed
// to its executor and answered at once. 'notificationP' (NULL: the registration's) - where the
// execution's changes are notified.
//
// Always answers the request (corRest.out / ldError); true.
//
extern bool seExecute(SeOrigin origin, const char* entityId, const char* serviceName, CorNode* inputP, CorNode* notificationP);



// -----------------------------------------------------------------------------
//
// seExecutionBuild - a simple execution, pending, not stored; NULL: refused (*statusP 404 / 400, why)
//
extern CorNode* seExecutionBuild(const char* entityId, const char* serviceName, CorNode* inputP, CorNode* notificationP, int* statusP, char* why, int whySize);



// -----------------------------------------------------------------------------
//
// seExecutionStart - a stored, pending simple execution handed to its executor; its outcome stored
//
extern void seExecutionStart(CorNode* execP, SeForward* resultP);



// -----------------------------------------------------------------------------
//
// seExecutionStore - an execution replaced in the database, and notified; a parent told of a child's end
//
extern bool seExecutionStore(Tenant* tenantP, CorNode* execP);



// -----------------------------------------------------------------------------
//
// seExecutionRetrieve - a stored execution; NULL after a 404/500 has been raised
//
extern CorNode* seExecutionRetrieve(const char* execId);



// -----------------------------------------------------------------------------
//
// seExecutionRender - an execution made ready for a response: no '_' members, sysAttrs or not
//
extern void seExecutionRender(CorNode* execP);



// -----------------------------------------------------------------------------
//
// seExecutionRenderAll - seExecutionRender, a combined or grouped execution's children embedded
//
extern void seExecutionRenderAll(CorNode* execP);



// -----------------------------------------------------------------------------
//
// seExecutionCancelOne - cancel one simple execution, answering no request
//
// 204: cancelled (or ended already), 409: its executor cannot pre-empt it, else the executor's failure
// (*resultP has the ProblemDetails).
//
extern int seExecutionCancelOne(CorNode* execP, SeForward* resultP);



// -----------------------------------------------------------------------------
//
// seExecutionUpdate - the executor's report (PATCH /services/{id}): status, progress, output, error
//
// Answers the request; true.
//
extern bool seExecutionUpdate(CorNode* execP, CorNode* updateP);



// -----------------------------------------------------------------------------
//
// seExecutionCancel - DELETE of an execution: a pending one cancelled, an executing one cancelled by its
// executor (409 when it cannot pre-empt), a finished one deleted. Answers the request; true.
//
extern bool seExecutionCancel(CorNode* execP);



// -----------------------------------------------------------------------------
//
// seExecutionSweep - the periodic work, per tenant: time-outs (failed) and retention (deleted)
//
extern void seExecutionSweep(Tenant* tenantP, int64_t nowNs);



// -----------------------------------------------------------------------------
//
// seExecutionTick - the periodic loop's (ldPeriodicLoopRegister): every 5 s, every tenant swept
//
extern void seExecutionTick(void* ctx, uint64_t nowNs, CorAlloc* kaP);



// -----------------------------------------------------------------------------
//
// seExecutionApplyBridge - a bridge's report on an asynchronous execution (bridgeServiceApplySet)
//
extern void seExecutionApplyBridge(const char* executionId, const char* status, const char* progressJson, const char* outputJson, const char* errorJson);

#endif  // SRC_LIB_SERVICEEXECUTION_SEEXECUTION_H_
