#ifndef BRIDGE_BRIDGESERVICESYNC_H_
#define BRIDGE_BRIDGESERVICESYNC_H_

//
// FILE            bridgeServiceSync.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // uint64_t, int64_t

#include "kjson/KjNode.h"                             // KjNode
#include "db/Tenant.h"                                // Tenant
#include "bridge/Channel.h"                           // Channel



// -----------------------------------------------------------------------------
//
// ddsSync - an NGSI-LD request that WAITS for the service it invokes
//
// By default, writing an attribute bound to a service invokes the service after
// the write and answers at once: the reply lands in the attribute whenever it
// comes, and a service that is not there is a line in the log. The NGSI-LD
// write succeeded on its own merits, and that is what the answer says.
//
// With ?ddsSync=true - or --ddsSync, which makes it the default and leaves
// ?ddsSync=false to opt out - the order is turned round: the service is invoked
// BEFORE the write, the request waits for THIS invocation's reply, and the value
// and the reply are then written together, in the request's own write. A
// service that cannot be reached, or does not answer in time, fails the request
// and NOTHING is written.
//
// ⭐ OFF BY DEFAULT, AND FOR TWO REASONS THAT ARE NOT ABOUT PERFORMANCE:
//
//   - it changes what a PATCH MEANS. Without a server, the asynchronous PATCH is
//     a 204 and the value is stored; the synchronous one is a 503 and nothing is.
//   - it changes the LATENCY of every such PATCH to that of the device behind
//     the service - which a client that did not ask for it did not agree to.
//
// The wait costs a corRest worker thread - the request is already on one, never
// on an event loop - and nothing else.
//
// Only the three entity PATCH forms take it: PATCH /entities/{id},
// /entities/{id}/attrs and /entities/{id}/attrs/{attrId}. On any other route
// ?ddsSync is an unknown parameter and the request is refused (400) - better
// than a client believing it had asked to wait when nothing would. Those routes
// invoke after the write, as always, and --ddsSync does not change them.
//



// -----------------------------------------------------------------------------
//
// BRIDGE_PARAM_DDS_SYNC - the URL parameter's bit in corRest's registry
//
// ⚠ The bits are one 64-bit space shared by every registrant. corNgsild's
// LD_PARAM_* grow upward from bit 0, so what the broker registers for itself
// is taken from the TOP, where the two cannot meet until the space is full.
//
#define BRIDGE_PARAM_DDS_SYNC  (1ULL << 63)



// -----------------------------------------------------------------------------
//
// bridgeSyncDefault / bridgeSyncTimeoutMs - the --ddsSync and --ddsSyncTimeout options
//
extern bool bridgeSyncDefault;
extern int  bridgeSyncTimeoutMs;



// -----------------------------------------------------------------------------
//
// BridgeSyncDone - the services a request has already invoked, synchronously
//
// Handed from the pre-write step to the post-write one, so that a service the
// request already waited for is not invoked a second time once the write is
// done. Lives on the handler's stack: the request is the whole of its lifetime.
//
#define BRIDGE_SYNC_MAX  16

typedef struct BridgeSyncDone
{
  Channel*  channelV[BRIDGE_SYNC_MAX];
  int       count;
} BridgeSyncDone;



// -----------------------------------------------------------------------------
//
// bridgeSyncRequested - does this request wait for its services?
//
// ?ddsSync=true|false if given, --ddsSync otherwise.
//
// @return false, with the error set, when ?ddsSync is neither 'true' nor 'false'.
//
extern bool bridgeSyncRequested(bool* syncP);



// -----------------------------------------------------------------------------
//
// bridgeSyncFragment - invoke, and wait for, every service a PATCH fragment writes
//
// Called by the three PATCH handlers after the fragment is in the DB model and
// before it is merged and written. For each attribute of the fragment that a
// service Channel carries, the service is invoked with the attribute's new value
// and the request waits for the reply, which is then added to the attribute as
// the sub-attribute the plugin names - exactly as an asynchronous reply would be
// stored - so that the ordinary write stores both.
//
// A no-op, returning true, unless the request asked to wait (bridgeSyncRequested).
//
// @param doneP  filled with the Channels invoked here; hand it to the post-write
//               bridgeAttrOut / bridgeAttrsOutFromMerge so they are not invoked
//               again.
//
// @return false, with the error set, when a service could not be reached (503),
//         did not answer in time (504), was sent a payload that does not fit it
//         (400), or is carried by a bridge that cannot wait (422). The handler
//         then returns without writing anything.
//
extern bool bridgeSyncFragment(Tenant* tenantP, const char* entityId, KjNode* fragmentP, BridgeSyncDone* doneP);



// -----------------------------------------------------------------------------
//
// bridgeSyncDoneHas - did this request already invoke the service on this Channel?
//
extern bool bridgeSyncDoneHas(const BridgeSyncDone* doneP, const Channel* channelP);



// -----------------------------------------------------------------------------
//
// bridgeReplyIn - BridgeBroker.replyIn, ABI 3
//
// A reply carrying the token of a request that waits for it is handed to that
// request. The reply to a request that already gave up is dropped. Anything
// else is an ordinary asynchronous reply, and goes where sampleQualifiedIn
// sends it.
//
extern int bridgeReplyIn(const char* bridgeName,
                         const char* endpoint,
                         uint64_t    token,
                         const char* datasetId,
                         const char* subAttrName,
                         const char* json,
                         int64_t     publishTime);

#endif  // BRIDGE_BRIDGESERVICESYNC_H_
