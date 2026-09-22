//
// FILE            bridgeSampleIn.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <string.h>                                   // memset
#include <time.h>                                     // clock_gettime

#include "kalloc/KAlloc.h"                            // KAlloc
#include "kalloc/kaBufferInit.h"                      // kaBufferInit
#include "kalloc/kaBufferReset.h"                     // kaBufferReset
#include "kalloc/kaAlloc.h"                           // kaAlloc
#include "kjson/kjBufferCreate.h"                     // kjBufferCreate
#include "kjson/kjParse.h"                            // kjParse
#include "kjson/kjBuilder.h"                          // kjObject, kjString, kjInteger, kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup
#include "ktrace/kTrace.h"                            // KT_T, KT_W

#include "corRest/corRest.h"                          // corRest
#include "corNgsild/CorNgsild.h"                      // corNgsild
#include "corJsonld/corLdExpandTree.h"                 // corLdExpandTree
#include "corJsonld/corLdInit.h"                       // corLdCoreContext
#include "corNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "corNgsild/LdNormalizeInput.h"                // ldNormalizeInput
#include "corNgsild/ldCheckEntity.h"                   // ldCheckEntity
#include "corNgsild/LdOp.h"                            // LdOpAppendAttrs
#include "corNgsild/ldEntityMerge.h"                  // LdMergeReport
#include "corNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityUpdate
#include "corNgsild/ldNotifyDefer.h"                  // ldNotifyDefer, ldNotifyDispatchPending
#include "corNgsild/ldCsrSubNotify.h"                 // ldCsrSubDispatchPending
#include "corNgsild/ldCheckSubscription.h"            // ldSubEntityTypeExprsRelease

#include "corBridge/BridgeBroker.h"                   // BRIDGE_OK, BRIDGE_NOT_FOUND, BRIDGE_BAD_INPUT

#include "db/DbDriver.h"                              // db, DB_OK
#include "troe/TroeDriver.h"                          // TroeEvent, TroeOpAttrReplaced, troeDispatchPending
#include "troe/troeDispatch.h"                        // troeDeferAttrEvent
#include "bridge/Channel.h"                           // Channel
#include "bridge/channelCache.h"                      // channelLookup
#include "bridge/bridgeSampleIn.h"                    // Own interface
#include "coraineTraceLevels.h"                       // KtBridge



// -----------------------------------------------------------------------------
//
// BRIDGE_SAMPLE_BUFFER - the per-thread arena a sample is assembled in
//
// Generous, because a sample carries whatever the publisher put on the wire and
// a point cloud is not a temperature. It is a per-thread cost paid once, not
// per sample.
//
#define BRIDGE_SAMPLE_BUFFER (256 * 1024)



// -----------------------------------------------------------------------------
//
// threadBind - make this plugin thread able to do broker work
//
// A transport runs threads of its own and hands a sample over on one of them.
// The broker did not create that thread, so its thread-locals - corRest.kalloc,
// corRest.kjsonP, corNgsild - are zeroed until somebody sets them up. That is
// this, once per thread, and then a reset per sample.
//
// ⚠️ KTRUE = REUSE, and it is not optional. kaBufferReset(kaP, KFALSE) is the
// TEARDOWN call: it frees the blocks and leaves allocList pointing at them, so
// reaching it a second time on the same arena walks a dangling list and frees
// the same pointers again.
//
static void threadBind(Channel* channelP)
{
  static __thread bool  inited = false;
  static __thread char  buffer[BRIDGE_SAMPLE_BUFFER];

  if (inited == false)
  {
    kaBufferInit(&corRest.kalloc, buffer, sizeof(buffer), 16 * 1024, NULL, "bridge");
    corRest.kjsonP = kjBufferCreate(&corRest.kjson, &corRest.kalloc);
    inited = true;
  }
  else
    kaBufferReset(&corRest.kalloc, KTRUE);

  //
  // The write runs AS the Channel's tenant. Not everything downstream takes a
  // tenant as a parameter, and a thread with no request has none.
  //
  corNgsild.tenantP = channelP->tenantP;

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  corRest.requestStartTime = (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}



// -----------------------------------------------------------------------------
//
// attributeFromSample - the payload, as an NGSI-LD Property
//
// The sample IS the value. What a publisher puts on an endpoint is the
// application's own data and the broker does not interpret it - it is stored
// whole, whether that is a number or a nested object.
//
// publishTime becomes observedAt, which is the term the spec already has for
// "when this was observed". A bespoke member would have said the same thing in
// a word only this broker understands.
//
static KjNode* attributeFromSample(const char* attrName, const char* json, int64_t publishTime)
{
  KjNode* valueP = kjParse(corRest.kjsonP, (char*) json);

  if (valueP == NULL)
    return NULL;

  KjNode* attrP = kjObject(corRest.kjsonP, attrName);

  kjChildAdd(attrP, kjString(corRest.kjsonP, "type", "Property"));

  valueP->name = (char*) "value";
  kjChildAdd(attrP, valueP);

  if (publishTime > 0)
    kjChildAdd(attrP, kjInteger(corRest.kjsonP, "observedAt", (long long) publishTime));

  return attrP;
}



// -----------------------------------------------------------------------------
//
// bridgeSampleIn -
//
int bridgeSampleIn(const char* bridgeName, const char* endpoint, const char* json, int64_t publishTime)
{
  if ((bridgeName == NULL) || (endpoint == NULL) || (json == NULL))
    return BRIDGE_BAD_INPUT;

  Channel* channelP = channelLookup(bridgeName, endpoint);

  //
  // Not an error. It means nobody asked for this endpoint - a transport that
  // hands over everything it hears will say a great deal the broker was never
  // configured to want.
  //
  if (channelP == NULL)
  {
    KT_T(KtBridge, "sample on '%s' from bridge '%s' - no channel claims it", endpoint, bridgeName);
    return BRIDGE_NOT_FOUND;
  }

  if (channelP->direction == BridgeDirectionOut)
  {
    KT_T(KtBridge, "sample on '%s' - the channel is outbound only", endpoint);
    return BRIDGE_NOT_FOUND;
  }

  threadBind(channelP);

  KjNode* attrP = attributeFromSample(channelP->attrName, json, publishTime);

  if (attrP == NULL)
  {
    KT_W("bridge '%s': the sample on '%s' is not valid JSON - dropped", bridgeName, endpoint);
    return BRIDGE_BAD_INPUT;
  }

  //
  // A fragment carries its attributes and nothing else. An 'id' in one becomes
  // '_id' in the DB model, and the write then does nothing while answering OK.
  //
  KjNode* fragmentP = kjObject(corRest.kjsonP, NULL);
  kjChildAdd(fragmentP, attrP);

  //
  // ⚠ EXPAND BEFORE CONVERTING, even though the names are already expanded.
  //
  // Expansion is not only about names: it stamps KJF_* flags onto every node,
  // and ldApiEntityToDbModel reads them to know which member of an attribute is
  // its VALUE. A hand-built tree has flags of zero, so an object-valued
  // Property is mistaken for a set of sub-attributes and gets createdAt and
  // modifiedAt written INSIDE the application's own data - where a GET then
  // returns them as though the publisher had sent them.
  //
  // A scalar value survives that, which is exactly why it has to be said out
  // loud: the bug is invisible until somebody publishes an object.
  //
  corLdExpandTree(fragmentP, corLdCoreContext(), &corRest.kalloc);

  ldApiEntityToDbModel(fragmentP, &corRest.kalloc, 0);

  //
  // ⚠ The report is NOT optional. The mongoc driver builds its entire $set by
  // walking reportP->changes, so a NULL report writes nothing and returns
  // DB_OK. It is also what subscription matching reads.
  //
  LdMergeReport report = { NULL };
  int           r      = db.entityAttrsSet(channelP->tenantP, channelP->entityId, fragmentP,
                                           false, corRest.requestStartTime, &report);

  if (r != DB_OK)
  {
    //
    // The entity is pre-created at startup for every Channel, so this is a
    // deployment whose entity was deleted while it was running, not an ordinary
    // first sample.
    //
    KT_W("bridge '%s': could not store the sample from '%s' into %s (%d)",
         bridgeName, endpoint, channelP->entityId, r);
    return BRIDGE_ERR;
  }

  //
  // The notification body is the whole entity, not the fragment - a subscriber
  // asked about an entity.
  //
  KjNode* mergedP = NULL;

  if (channelP->tenantP->subCacheP != NULL)
  {
    db.entityRetrieve(channelP->tenantP, channelP->entityId, &mergedP);

    if (mergedP != NULL)
      ldNotifyDefer((LdSubCache*) channelP->tenantP->subCacheP, mergedP, LdNotifyEntityUpdate, &report);
  }

  if (troe.attrEvent != NULL || troe.eventList != NULL)
  {
    TroeEvent* tevP = (TroeEvent*) kaAlloc(&corRest.kalloc, sizeof(TroeEvent));

    memset(tevP, 0, sizeof(TroeEvent));
    tevP->op             = TroeOpAttrReplaced;
    tevP->tenantP        = channelP->tenantP;
    tevP->entityId       = channelP->entityId;
    tevP->entityType     = channelP->entityType;
    tevP->attrName       = channelP->attrName;
    tevP->modifiedAtNs   = corRest.requestStartTime;
    tevP->entitySnapshot = mergedP;
    troeDeferAttrEvent(tevP);
  }

  //
  // ⭐⭐ AND NOW DRAIN, WHICH NOTHING ELSE WILL.
  //
  // Notification and TRoE are deferred onto queues that brokerPostResponseHook
  // empties once an HTTP response has been sent. A sample has no request behind
  // it and no response ahead of it, so without this the write lands in the
  // store and then nothing notifies and no history is recorded - silently, and
  // looking exactly like a subscription that does not match.
  //
  // Only the queues this write filled. The rest of that hook belongs to a
  // request: metrics count requests, and the expired-entity and registration
  // probes are read-path work.
  //
  ldNotifyDispatchPending();
  ldCsrSubDispatchPending();
  troeDispatchPending();
  ldSubEntityTypeExprsRelease();

  KT_T(KtBridge, "sample on '%s' -> %s/%s", endpoint, channelP->entityId, channelP->attrName);

  return BRIDGE_OK;
}
