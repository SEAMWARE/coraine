//
// FILE            bridgeSampleIn.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <string.h>                                   // memset, strlen, strcpy, strcat
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
#include "corJsonld/CorLdContext.h"                  // CorLdContext
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
#include "bridge/channelCache.h"                      // channelLookup, channelLookupByTarget
#include "bridge/bridgeDefaultEntity.h"               // bridgeDefaultEntityGet
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
static void threadBind(Tenant* tenantP)
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
  // The write runs AS the target's tenant. Not everything downstream takes a
  // tenant as a parameter, and a thread with no request has none.
  //
  corNgsild.tenantP = tenantP;

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

  Channel*    channelP   = channelLookup(bridgeName, endpoint);
  Tenant*     tenantP    = NULL;
  const char* entityId   = NULL;
  const char* entityType = NULL;
  const char* attrName   = NULL;
  bool        catchAll   = false;

  if (channelP != NULL)
  {
    if (channelP->direction == BridgeDirectionOut)
    {
      KT_T(KtBridge, "sample on '%s' - the channel is outbound only", endpoint);
      return BRIDGE_NOT_FOUND;
    }

    tenantP    = channelP->tenantP;
    entityId   = channelP->entityId;
    entityType = channelP->entityType;
    attrName   = channelP->attrName;
  }
  //
  // Nobody asked for this endpoint. A transport that hands over everything it
  // hears says a great deal the broker was never configured to want, and
  // dropping it is the default - unless this bridge was given a catch-all, in
  // which case the endpoint names its own attribute.
  //
  else if (bridgeDefaultEntityGet(bridgeName, &entityId, &entityType, &tenantP) == true)
    catchAll = true;
  else
  {
    KT_T(KtBridge, "sample on '%s' from bridge '%s' - no channel claims it", endpoint, bridgeName);
    return BRIDGE_NOT_FOUND;
  }

  threadBind(tenantP);

  //
  // The endpoint IS the attribute name here, and it is put under @vocab
  // DIRECTLY rather than expanded.
  //
  // ⭐ Expanding it is what the Channel path does, and it is wrong here, twice
  // over. A Channel's attribute is a name a person wrote in a configuration
  // file, meaning it in NGSI-LD; an endpoint is what a foreign system calls
  // one of its own things, and the broker is quoting it, not adopting it.
  //
  //   - Expansion VALIDATES, against the § 4.6.2 NGSI-LD Name grammar, and
  //     'rt/chatter' fails it on the slash. Every ROS 2 topic carries one
  //     (ROS prefixes its topics with "rt/"), so the catch-all would refuse
  //     precisely the system it was built to show. The rule is should-level
  //     and is about names the API is asked to accept - this is a name it is
  //     reporting.
  //   - Expansion also LOOKS THE NAME UP, so a topic that happens to be called
  //     'location' would land on the core context's GeoProperty term, with its
  //     value checks, because of what someone else named a topic.
  //
  // ⚠ The consequence, and it is the honest one: such an attribute cannot be
  // addressed through /entities/{id}/attrs/{attr} - a slash is a path
  // separator. It is readable on the entity and it is inbound only anyway, the
  // catch-all being a view of what a system publishes rather than a way to
  // write to it.
  //
  if (catchAll == true)
  {
    CorLdContext* coreP = corLdCoreContext();

    if ((coreP == NULL) || (coreP->vocab == NULL))
    {
      KT_W("bridge '%s': no @vocab to name '%s' under - sample dropped", bridgeName, endpoint);
      return BRIDGE_BAD_INPUT;
    }

    int   len   = strlen(coreP->vocab) + strlen(endpoint) + 1;
    char* nameP = (char*) kaAlloc(&corRest.kalloc, len);

    if (nameP == NULL)
      return BRIDGE_ERR;

    strcpy(nameP, coreP->vocab);
    strcat(nameP, endpoint);

    attrName = nameP;

    //
    // ⚠ And it must not land on an attribute a Channel already writes.
    //
    // The catch-all's entity is its own by default, so this needs a file that
    // pointed defaultEntity at an entity Channels also write, and then an
    // unclaimed endpoint whose name matches one of their attributes. Rare, and
    // silent: the topic's value would overwrite the Channel's, and the
    // attribute's value would depend on which arrived last.
    //
    // That is exactly the collision channelCreate refuses at configuration
    // time ("they would race, and the value would depend on which arrived
    // last"), and the same rule has to hold for a writer the configuration
    // never named. Refused with the same words, in a trace rather than a fatal
    // - this one is a sample arriving, not a broker starting.
    //
    Channel* clashP = channelLookupByTarget(tenantP, entityId, attrName);

    if (clashP != NULL)
    {
      KT_T(KtBridge, "sample on '%s' would write %s/%s, which channel '%s' already writes - dropped",
           endpoint, entityId, attrName, clashP->endpoint);
      return BRIDGE_NOT_FOUND;
    }
  }

  KjNode* attrP = attributeFromSample(attrName, json, publishTime);

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
  // The catch-all's entity is made here, on the first sample that needs it,
  // because until one arrived there was nothing to say it was needed. A
  // Channel's entity was created at startup from the configuration.
  //
  if ((catchAll == true) && (bridgeDefaultEntityNeedsCreate(bridgeName) == true))
  {
    KjNode* existingP = NULL;

    if ((db.entityRetrieve == NULL) || (db.entityRetrieve(tenantP, entityId, &existingP) != DB_OK) || (existingP == NULL))
    {
      KjNode* entityP = kjObject(corRest.kjsonP, NULL);

      kjChildAdd(entityP, kjString(corRest.kjsonP, "id",   (char*) entityId));
      kjChildAdd(entityP, kjString(corRest.kjsonP, "type", (char*) entityType));

      ldApiEntityToDbModel(entityP, &corRest.kalloc, 0);

      if ((db.entityCreate == NULL) || (db.entityCreate(tenantP, entityId, entityP) != DB_OK))
        KT_W("bridge '%s': could not create the catch-all entity '%s'", bridgeName, entityId);
    }

    //
    // Marked either way. A second attempt would fail for the same reason, and
    // the store below reports it per sample in any case.
    //
    bridgeDefaultEntityCreated(bridgeName);
  }

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
  int           r      = db.entityAttrsSet(tenantP, entityId, fragmentP,
                                           false, corRest.requestStartTime, &report);

  if (r != DB_OK)
  {
    //
    // The entity is pre-created at startup for every Channel, so this is a
    // deployment whose entity was deleted while it was running, not an ordinary
    // first sample.
    //
    KT_W("bridge '%s': could not store the sample from '%s' into %s (%d)",
         bridgeName, endpoint, entityId, r);
    return BRIDGE_ERR;
  }

  //
  // The notification body is the whole entity, not the fragment - a subscriber
  // asked about an entity.
  //
  KjNode* mergedP = NULL;

  if (tenantP->subCacheP != NULL)
  {
    db.entityRetrieve(tenantP, entityId, &mergedP);

    if (mergedP != NULL)
      ldNotifyDefer((LdSubCache*) tenantP->subCacheP, mergedP, LdNotifyEntityUpdate, &report);
  }

  if (troe.attrEvent != NULL || troe.eventList != NULL)
  {
    TroeEvent* tevP = (TroeEvent*) kaAlloc(&corRest.kalloc, sizeof(TroeEvent));

    memset(tevP, 0, sizeof(TroeEvent));
    tevP->op             = TroeOpAttrReplaced;
    tevP->tenantP        = tenantP;
    tevP->entityId       = entityId;
    tevP->entityType     = entityType;
    tevP->attrName       = attrName;
    tevP->modifiedAtNs   = corRest.requestStartTime;

    //
    // ⭐ attrSnapshot is what the VALUE is read from, and leaving it NULL is
    // how a sample got a history row saying an attribute was replaced and not
    // saying what with - every v_* column empty in the timescale table.
    //
    // fragmentP is the tree that was just written, dataset-keyed by
    // ldApiEntityToDbModel, which is exactly the shape the plugin walks
    // (extractCols takes the wrapper and reads its first instance). It is in
    // hand already, so this costs nothing: the alternative, a retrieve, is
    // what entitySnapshot below does and it is why that one is conditional.
    //
    tevP->attrSnapshot   = kjLookup(fragmentP, attrName);

    //
    // ⚠ And entitySnapshot is NULL when nothing subscribes, because that is
    // the only reason the entity is fetched at all. An attribute event does
    // not need it - the plugin reads attrSnapshot - but it must not be the
    // thing the value depends on, which is what it silently was.
    //
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

  KT_T(KtBridge, "sample on '%s' -> %s/%s", endpoint, entityId, attrName);

  return BRIDGE_OK;
}
