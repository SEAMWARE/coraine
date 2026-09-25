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
// Requests to the DDS side - DDS FIRST
//
// ⭐ A TOPIC REPORTS A FACT; A SERVICE OR AN ACTION ASKS FOR SOMETHING TO BE DONE.
// A write that publishes on a topic is NGSI-LD first: the value is stored, then
// published, and a publish that fails is the transport's problem. A write bound
// to a service or an action asks somebody on the DDS side to act, and that side
// is the master of it: storing the request as made when it never went out would
// be a lie. So the request is sent BEFORE anything is stored, and one that
// cannot be sent fails the NGSI-LD request with nothing written:
//
//   503  nobody serves the endpoint        400  the value does not fit its type
//   422  the bridge cannot carry this at all
//
// ⭐ ONE ATTRIBUTE OR SEVERAL. The above is a request that writes ONE attribute.
// One that writes several is not held hostage by one of them: it never waits,
// and an attribute whose request cannot be sent is taken out and not written -
// the rest is, and the answer is 207 with that attribute not updated.
//
// ⭐⭐ AN ACTION'S TRANSPORT DECIDES BEFORE ANYTHING IS STORED. Its goal is sent,
// and the request WAITS for the goal's first event - on every route, whatever
// ?ddsSync says - and writes the value only for a goal that was accepted,
// together with that first event, as the goal's own instance. A goal can be
// refused for reasons nobody sees until it is, and a value stored for one would
// be a record of something that is not happening:
//
//   422  rejected (errorCode goalRejected)   503  the transport lost it (goalFailed)
//   504  no answer by --ddsSyncTimeout (goalNotAnswered) - and the goal is
//        CANCELLED, so that "nothing was written" stays true
//
// Several goals are all sent first and then waited for against ONE deadline,
// and each attribute is written or refused on its own goal's answer (207). A
// batch sends EVERY Entity's goals before it waits for any (BRIDGE_REQ_SEND_ONLY,
// then bridgeRequestsAwait): its wait is its slowest goal, never their sum.
//
// What a request that DID go out answers:
//
//   an action goal, accepted    202 - accepted, not done: a goal runs, and its
//                               events land in an instance of its own
//   a service, waited for,      as any write (204) - the reply is written WITH
//   answered in time            the value, in the request's own write
//   a service, not waited for,  202 - the reply lands in the attribute when it
//   or not answered in time     comes (GET to poll, or subscribe)
//
// ?ddsSync=true|false (or --ddsSync as the default) says whether to wait for a
// service at all. The wait is short - --ddsSyncTimeout, sized for the normal
// case - and CAPPED: a waiting request holds a corRest worker, and the pool is
// small, so at most --ddsSyncWaitMax requests wait at once and the rest are
// sent without waiting (202). A DDS network that is slow, or gone, can then never
// take the broker's workers from everything else.
//
// EVERY write sends before it stores: create, append, the three PATCH forms,
// replace of an Attribute and of an Entity, and the batch operations (per
// Entity, released after the bulk write, BRIDGE_REQ_PER_ENTITY). Nothing is sent
// to a service or an action after a write any more - bridgeAttrOut publishes
// topics only. Only the PATCH forms may WAIT (?ddsSync is theirs; on any other
// route it is an unknown parameter, refused with 400).
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
// bridgeSyncDefault / bridgeSyncTimeoutMs / bridgeSyncWaitMax - --ddsSync,
// --ddsSyncTimeout and --ddsSyncWaitMax
//
extern bool bridgeSyncDefault;
extern int  bridgeSyncTimeoutMs;
extern int  bridgeSyncWaitMax;



// -----------------------------------------------------------------------------
//
// BridgeSyncDone - what a request has already sent to the DDS side, before its write
//
// Handed from the pre-write step to the post-write one, so that nothing sent
// before the write is sent a second time after it, and so that the handler knows
// to answer 202. Lives on the handler's stack: the request is the whole of its
// lifetime.
//
#define BRIDGE_SYNC_MAX  16

typedef struct BridgeSyncDone
{
  Channel*  channelV[BRIDGE_SYNC_MAX];
  int       count;
  bool      accepted;                                 // something went out that is not finished - answer 202
  uint64_t  detachedV[BRIDGE_SYNC_MAX];               // per channelV: the token of a wait that timed out, else 0
  uint64_t  goalV[BRIDGE_SYNC_MAX];                   // per channelV: the token of a goal held for the write, else 0
  KjNode*   goalAttrV[BRIDGE_SYNC_MAX];               // per goal: its attribute in the fragment - the instance goes in there
  char*     goalRequestV[BRIDGE_SYNC_MAX];            // per goal: the goal as sent - its instance's value
  char*     goalIdV[BRIDGE_SYNC_MAX];                 // per goal: the transport's id, once accepted (POST /channels/{id}/goals)
  bool      several;                                  // a refusal takes the attribute out, and does not fail the request

  //
  // A request writing SEVERAL attributes does not fail as a whole because one
  // request to the DDS side could not be sent: that attribute is taken out of
  // the fragment and not written, the rest is, and the handler reports it here
  // as not updated (207). See "Requests to the DDS side".
  //
  int          failedN;
  const char*  failedAttrV[BRIDGE_SYNC_MAX];          // the attribute - not written
  int          failedStatusV[BRIDGE_SYNC_MAX];        // what a request of it alone would have answered
  const char*  failedReasonV[BRIDGE_SYNC_MAX];        // why, in the request's arena
  const char*  failedTitleV[BRIDGE_SYNC_MAX];         // the ProblemDetails title of that status
  const char*  failedTypeV[BRIDGE_SYNC_MAX];          // ... and its type
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
// bridgeRequestsBeforeWrite - send every service request and goal a fragment makes
//
// Called by every single-entity write handler after the fragment is in the DB
// model and before it is merged and written.
//
// @param flags  BRIDGE_REQ_MAY_WAIT: a service may be waited for - the three
//               PATCH forms, whose ?ddsSync this is. Every other route sends
//               without waiting.
//               BRIDGE_REQ_PER_ENTITY: a batch operation. A request that cannot
//               be sent never fails the call, not even for an Entity writing a
//               single Attribute: it is recorded and left out, and the batch
//               reports it as that Entity's error - one unreachable robot does
//               not fail a batch of five hundred. See "Requests to the DDS side" above for what
// is sent and what the request then answers; a reply waited for is added to its
// attribute as the sub-attribute the plugin names, so the ordinary write stores
// both.
//
// @param doneP  filled with what was sent. Hand it to the post-write
//               bridgeAttrOut / bridgeAttrsOutFromMerge so nothing is sent
//               twice, to bridgeRequestsWritten once the write is done, and read
//               doneP->accepted for 202.
//
// @return false, with the error set, when a request could not be sent. The
//         handler then returns without writing anything.
//
#define BRIDGE_REQ_MAY_WAIT    0x1                    // a service may be waited for (?ddsSync) - the PATCH forms
#define BRIDGE_REQ_PER_ENTITY  0x2                    // a batch: nothing fails the call - see below
#define BRIDGE_REQ_SEND_ONLY   0x4                    // goals are sent, not waited for - the caller calls bridgeRequestsAwait

extern bool bridgeRequestsBeforeWrite(Tenant* tenantP, const char* entityId, KjNode* fragmentP, int flags, BridgeSyncDone* doneP);



// -----------------------------------------------------------------------------
//
// bridgeRequestsAwait - wait for the goals a fragment sent, and write only those accepted
//
// bridgeRequestsBeforeWrite does this itself, unless BRIDGE_REQ_SEND_ONLY. A
// batch sends every Entity's goals first and then calls this once per Entity,
// all with the same dueMs (bridgeRequestsDeadline): a goal's answer never waits
// for the next goal to be sent.
//
// An accepted goal's instance - the request and the first event - is put in
// its attribute in the fragment, for the request's one write to create. A
// refused or unanswered goal's attribute is taken out of the fragment and
// recorded as failed (in a batch, a request never fails as a whole).
//
// @return false, with the error set, when the request fails as a whole.
//
extern bool bridgeRequestsAwait(KjNode* fragmentP, BridgeSyncDone* doneP, int64_t dueMs);



// -----------------------------------------------------------------------------
//
// bridgeRequestsDeadline - the absolute deadline of a wait begun now (--ddsSyncTimeout)
//
extern int64_t bridgeRequestsDeadline(void);



// -----------------------------------------------------------------------------
//
// bridgeRequestsWritten - the request's write is done: late replies may land now
//
// A service that did not answer in time answers later, and its reply must not
// be written before the request's own write, which would replace it away. Call
// this right after the write - whether it succeeded or not.
//
extern void bridgeRequestsWritten(const BridgeSyncDone* doneP);



// -----------------------------------------------------------------------------
//
// bridgeRequestsReleasePending - the request's notifications have gone out: its goals may speak
//
// For the post-response hook, AFTER ldNotifyDispatchPending (which sends
// inline). bridgeRequestsWritten queued the goals the request holds; their
// events wait until now, so that none of them reaches a subscriber before the
// request's own notification of the write that made the goal's instance.
//
extern void bridgeRequestsReleasePending(void);



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



// -----------------------------------------------------------------------------
//
// bridgeReplyMetaIn - BridgeBroker.replyMetaIn, ABI 6: bridgeReplyIn, meta on the reply's sub-attribute
//
extern int bridgeReplyMetaIn(const char* bridgeName,
                             const char* endpoint,
                             uint64_t    token,
                             const char* datasetId,
                             const char* subAttrName,
                             const char* json,
                             const char* meta,
                             int64_t     publishTime);



// -----------------------------------------------------------------------------
//
// bridgeReplyExchangeIn - BridgeBroker.replyExchangeIn, ABI 7
//
// bridgeReplyMetaIn(), and the request it answers as a sub-attribute of its
// own, in the same write.
//
extern int bridgeReplyExchangeIn(const char* bridgeName,
                                 const char* endpoint,
                                 uint64_t    token,
                                 const char* datasetId,
                                 const char* requestSubAttrName,
                                 const char* requestJson,
                                 const char* requestMeta,
                                 int64_t     requestTime,
                                 const char* subAttrName,
                                 const char* json,
                                 const char* meta,
                                 int64_t     publishTime);

#endif  // BRIDGE_BRIDGESERVICESYNC_H_
