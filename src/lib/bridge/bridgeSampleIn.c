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
#include "kalloc/kaStrdup.h"                          // kaStrdup
#include "kjson/kjBufferCreate.h"                     // kjBufferCreate
#include "kjson/kjParse.h"                            // kjParse
#include "kjson/kjBuilder.h"                          // kjObject, kjString, kjInteger, kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjClone.h"                            // kjClone
#include "ktrace/kTrace.h"                            // KT_T, KT_W

#include "corRest/corRest.h"                          // corRest
#include "corNgsild/CorNgsild.h"                      // corNgsild, ldDefaultContext
#include "corJsonld/CorLdContext.h"                  // CorLdContext
#include "corJsonld/corLdExpandTree.h"                 // corLdExpandTree
#include "corJsonld/corLdInit.h"                       // corLdCoreContext
#include "corNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "corNgsild/LdNormalizeInput.h"                // ldNormalizeInput
#include "corNgsild/ldCheckEntity.h"                   // ldCheckEntity
#include "corNgsild/LdOp.h"                            // LdOpAppendAttrs
#include "corNgsild/ldEntityMerge.h"                  // LdMergeReport
#include "corNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityUpdate
#include "corNgsild/ldHooks.h"                        // corNgsildFallbackRelease
#include "corNgsild/ldNotifyDefer.h"                  // ldNotifyDefer, ldNotifyDispatchPending
#include "corNgsild/ldCsrSubNotify.h"                 // ldCsrSubDispatchPending
#include "corNgsild/ldCheckSubscription.h"            // ldSubEntityTypeExprsRelease

#include "corBridge/BridgeBroker.h"                   // BRIDGE_OK, BRIDGE_NOT_FOUND, BRIDGE_BAD_INPUT
#include "corBridge/corBridge.h"                      // corBridgeKindName

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
// ⭐ WITH A subAttrName IT IS THE OTHER WAY AROUND, and that asymmetry is the
// whole of what a reply is. The attribute's value is what was ASKED - a request
// somebody wrote through the API - and it must survive the answer arriving. So
// the payload goes one level down, into a sub-attribute of the name the plugin
// chose, and the value is carried across unchanged.
//
// Which is also why observedAt then belongs to the SUB-attribute alone: the
// reply was observed now, the request was not, and stamping the outer attribute
// would say the value had just been written when nothing had touched it.
//
// @param existingValueP  the attribute's current value, from the store. CLONED
//                        rather than moved: kjChildAdd relinks a node into its
//                        new parent and truncates the list it came from, and
//                        that list is a tree the caller still reads.
//
static KjNode* attributeFromSample(const char* attrName,
                                   const char* json,
                                   int64_t     publishTime,
                                   const char* datasetId,
                                   const char* subAttrName,
                                   KjNode*     existingValueP)
{
  KjNode* payloadP = kjParse(corRest.kjsonP, (char*) json);

  if (payloadP == NULL)
    return NULL;

  KjNode* attrP = kjObject(corRest.kjsonP, attrName);

  kjChildAdd(attrP, kjString(corRest.kjsonP, "type", "Property"));

  if (subAttrName == NULL)
  {
    payloadP->name = (char*) "value";
    kjChildAdd(attrP, payloadP);

    if (publishTime > 0)
      kjChildAdd(attrP, kjInteger(corRest.kjsonP, "observedAt", (long long) publishTime));
  }
  else
  {
    KjNode* valueP = kjClone(corRest.kjsonP, existingValueP);

    if (valueP == NULL)
      return NULL;

    valueP->name = (char*) "value";
    kjChildAdd(attrP, valueP);

    KjNode* subP = kjObject(corRest.kjsonP, subAttrName);

    kjChildAdd(subP, kjString(corRest.kjsonP, "type", "Property"));
    payloadP->name = (char*) "value";
    kjChildAdd(subP, payloadP);

    if (publishTime > 0)
      kjChildAdd(subP, kjInteger(corRest.kjsonP, "observedAt", (long long) publishTime));

    kjChildAdd(attrP, subP);
  }

  //
  // Last, so that removing it in extractDatasetId leaves the rest in the order
  // it was built in - which is the order a GET returns it in.
  //
  if (datasetId != NULL)
    kjChildAdd(attrP, kjString(corRest.kjsonP, "datasetId", (char*) datasetId));

  return attrP;
}



// -----------------------------------------------------------------------------
//
// attrInstance - what the store currently holds for one instance
//
// The DB model keys an attribute by datasetId - "attr": { "@none": { ... } } -
// so this is two lookups. NULL when the entity, the attribute or that
// particular instance is not there.
//
// The whole instance and not only its value, because everything on it has to
// survive an answer arriving - see instanceCarryOver.
//
static KjNode* attrInstance(Tenant* tenantP, const char* entityId, const char* attrName, const char* datasetId)
{
  KjNode* entityP = NULL;

  if (db.entityRetrieve == NULL)
    return NULL;

  if ((db.entityRetrieve(tenantP, entityId, &entityP) != DB_OK) || (entityP == NULL))
    return NULL;

  KjNode* attrP = kjLookup(entityP, attrName);

  if (attrP == NULL)
    return NULL;

  return kjLookup(attrP, (datasetId != NULL) ? datasetId : "@none");
}



// -----------------------------------------------------------------------------
//
// instanceCarryOver - keep what the answer did not come to change
//
// ⭐ A REPLY IS AN ADDITION, NOT A REPLACEMENT, and the store's only way to
// write an attribute instance is to replace it. So everything the instance
// already had has to be put back: its unitCode, its observedAt, and any
// sub-attribute somebody wrote alongside the request.
//
// Without this they were all silently dropped the moment an answer arrived -
// the attribute came back holding its value and the reply and nothing else.
//
// ⭐ DONE AFTER THE CONVERSION, WHICH IS WHAT MAKES IT SIMPLE. Both trees are
// then in the DB model, so names are expanded on both sides and comparing them
// works - including for the sub-attribute this arrival is itself writing, whose
// previous value is skipped rather than duplicated. And the members the
// conversion has just written - type, value, createdAt, modifiedAt - are
// skipped for free by the same rule: anything already there stays.
//
static void instanceCarryOver(KjNode* newInstanceP, KjNode* oldInstanceP)
{
  if ((newInstanceP == NULL) || (oldInstanceP == NULL))
    return;

  for (KjNode* childP = oldInstanceP->value.firstChildP; childP != NULL; childP = childP->next)
  {
    if (kjLookup(newInstanceP, childP->name) != NULL)
      continue;

    KjNode* copyP = kjClone(corRest.kjsonP, childP);

    if (copyP != NULL)
      kjChildAdd(newInstanceP, copyP);
  }
}



// -----------------------------------------------------------------------------
//
// sampleIn - the one inbound path, qualified or not
//
// bridgeSampleIn() and bridgeSampleQualifiedIn() are this function with and
// without the two qualifiers. Everything after the attribute has been built is
// identical - the store, the notification, the temporal event, the drain - and
// duplicating it for a reply would have meant two copies of the one piece of
// code in the broker that runs on a thread it did not create.
//
// @param seedJson  a GOAL's request, as text, or NULL. A goal's instance does
//                  not exist until the first event about it arrives - the
//                  broker did not know the goal's alias before that - so this
//                  is the value the instance is created with. Text and not a
//                  tree, because the arena is reset below and it has to be
//                  parsed after that.
// @param goal      the call is about a goal (bridgeGoal.c), which may write an
//                  action Channel's attribute without a sub-attribute: the
//                  null that removes a finished goal's instance.
//
static int sampleIn(const char* bridgeName,
                    const char* endpoint,
                    const char* datasetId,
                    const char* subAttrName,
                    const char* json,
                    int64_t     publishTime,
                    const char* seedJson,
                    bool        goal)
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
    //
    // ⭐ "Outbound only" is a statement about SAMPLES, and a reply is not one.
    //
    // A service Channel is outbound because the broker is the one that asks -
    // that is what being a client means - and the answer coming back is the
    // other half of the very exchange the direction describes. Refusing it here
    // would mean no reply could ever arrive on the only kind of Channel that
    // can produce one.
    //
    // A plain sample on such a Channel is a different matter: nothing on a
    // request/reply endpoint publishes unprompted, so one arriving is a plugin
    // handing over something it has mislabelled, and it is dropped.
    //
    if (channelP->kind == BridgeChannelTopic)
    {
      if (channelP->direction == BridgeDirectionOut)
      {
        KT_T(KtBridge, "sample on '%s' - the channel is outbound only", endpoint);
        return BRIDGE_NOT_FOUND;
      }
    }
    else if ((subAttrName == NULL) && (goal == false))
    {
      KT_T(KtBridge, "unqualified sample on '%s', which is a %s channel - dropped",
           endpoint, corBridgeKindName(channelP->kind));
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
  //
  // ⚠ And a qualified arrival is never a catch-all's. The catch-all shows what
  // a system PUBLISHES; a reply is an answer to something this broker sent, so
  // an unclaimed endpoint delivering one means the Channel that sent the
  // request has gone - not that a new attribute should appear.
  //
  else if ((subAttrName == NULL) && (bridgeDefaultEntityGet(bridgeName, &entityId, &entityType, &tenantP) == true))
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
    //
    // The @vocab of whatever context this deployment expands with - the
    // default user context when it has one, core otherwise. A user context may
    // define its own @vocab, and an endpoint quoted under the wrong one is an
    // attribute nobody else names the same way.
    //
    CorLdContext* coreP = ldDefaultContext(&corRest.kalloc);

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

  //
  // What the attribute holds right now, which only a qualified arrival needs:
  // a reply is added TO an attribute, not written over it.
  //
  KjNode* existingInstanceP = NULL;
  KjNode* existingValueP     = NULL;

  if (subAttrName != NULL)
  {
    existingInstanceP = attrInstance(tenantP, entityId, attrName, datasetId);
    existingValueP    = (existingInstanceP != NULL) ? kjLookup(existingInstanceP, "value") : NULL;

    //
    // A goal's first event: the instance is created here, holding what was
    // asked. It is the one arrival that may make an instance, because the
    // broker sent the goal and knows what it was - the "answer to nothing" below
    // is exactly the case where it does not.
    //
    if ((existingValueP == NULL) && (seedJson != NULL))
    {
      existingValueP    = kjParse(corRest.kjsonP, kaStrdup(&corRest.kalloc, seedJson));
      existingInstanceP = NULL;

      if (existingValueP == NULL)
        return BRIDGE_BAD_INPUT;
    }

    if (existingValueP == NULL)
    {
      //
      // The instance the answer belongs to is gone - the entity deleted, or the
      // attribute removed, while the exchange was in flight. Saying so is the
      // point: the alternative is an attribute that appears out of nowhere with
      // a reply in it and no record of what was asked.
      //
      KT_W("bridge '%s': '%s' answered on '%s', but %s/%s%s%s no longer holds anything to answer - dropped",
           bridgeName, subAttrName, endpoint, entityId, attrName,
           (datasetId != NULL) ? " dataset " : "", (datasetId != NULL) ? datasetId : "");
      return BRIDGE_NOT_FOUND;
    }
  }

  KjNode* attrP = attributeFromSample(attrName, json, publishTime, datasetId, subAttrName, existingValueP);

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
  // And everything the instance already had, which the write below would
  // otherwise replace away. Both trees are in the DB model by now, which is the
  // only point at which their names can be compared.
  //
  if (subAttrName != NULL)
  {
    KjNode* wrapperP = kjLookup(fragmentP, attrName);

    if (wrapperP != NULL)
      instanceCarryOver(kjLookup(wrapperP, (datasetId != NULL) ? datasetId : "@none"), existingInstanceP);
  }

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

  //
  // ⚠ And give back what the queues grew to. This thread has no connection, so
  // its corNgsild is the per-thread fallback, and nothing frees that - a plugin
  // thread that ended took its queues with it, 640 bytes definitely lost in
  // every valgrind run with a bridge.
  //
  corNgsildFallbackRelease();

  if (subAttrName == NULL)
    KT_T(KtBridge, "sample on '%s' -> %s/%s", endpoint, entityId, attrName);
  else
    KT_T(KtBridge, "'%s' on '%s' -> %s/%s.%s", subAttrName, endpoint, entityId, attrName, subAttrName);

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// bridgeReplySubAttr - a reply, as the sub-attribute it is stored as
//
// ⭐ THE ONE WAY A REPLY BECOMES A SUB-ATTRIBUTE, whichever path it takes. An
// asynchronous reply is built, expanded and converted by sampleIn() below; a
// reply a request waited for (ddsSync) is grafted into that request's own
// fragment instead - and the two must be stored identically, or the same answer
// would read differently depending on who asked for it. So this runs the very
// steps sampleIn() runs, on a throwaway attribute, and hands back the part that
// is the reply.
//
// The attribute's value is a placeholder: the caller's fragment has the real
// one, and only the sub-attribute is taken from here.
//
// @return the sub-attribute, in the DB model and unlinked from anything, or
//         NULL when the reply is not valid JSON.
//
KjNode* bridgeReplySubAttr(const char* attrName, const char* subAttrName, const char* json, int64_t publishTime)
{
  //
  // ⚠ kjParse parses IN PLACE - every name and string in the tree points into
  // the text it was given. The caller's text is not the request's (a reply that
  // was waited for arrives in a buffer freed as soon as it has been grafted), so
  // the tree gets a copy of its own, in the arena it lives in.
  //
  char*   jsonCopy     = kaStrdup(&corRest.kalloc, json);
  KjNode* placeholderP = kjString(corRest.kjsonP, NULL, "-");
  KjNode* attrP        = attributeFromSample(attrName, jsonCopy, publishTime, NULL, subAttrName, placeholderP);

  if (attrP == NULL)
    return NULL;

  KjNode* fragmentP = kjObject(corRest.kjsonP, NULL);
  kjChildAdd(fragmentP, attrP);

  corLdExpandTree(fragmentP, corLdCoreContext(), &corRest.kalloc);
  ldApiEntityToDbModel(fragmentP, &corRest.kalloc, 0);

  KjNode* wrapperP  = kjLookup(fragmentP, attrName);
  KjNode* instanceP = (wrapperP != NULL) ? kjLookup(wrapperP, "@none") : NULL;

  if (instanceP == NULL)
    return NULL;

  //
  // The reply is the one object in the instance that is not its value - the
  // name it was given has been expanded on the way, so it is found by shape.
  //
  for (KjNode* nodeP = instanceP->value.firstChildP; nodeP != NULL; nodeP = nodeP->next)
  {
    if ((nodeP->type == KjObject) && (strcmp(nodeP->name, "value") != 0))
    {
      kjChildRemove(instanceP, nodeP);
      nodeP->next = NULL;
      return nodeP;
    }
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// bridgeSampleIn -
//
int bridgeSampleIn(const char* bridgeName, const char* endpoint, const char* json, int64_t publishTime)
{
  return sampleIn(bridgeName, endpoint, NULL, NULL, json, publishTime, NULL, false);
}



// -----------------------------------------------------------------------------
//
// bridgeSampleQualifiedIn -
//
int bridgeSampleQualifiedIn(const char* bridgeName,
                            const char* endpoint,
                            const char* datasetId,
                            const char* subAttrName,
                            const char* json,
                            int64_t     publishTime)
{
  return sampleIn(bridgeName, endpoint, datasetId, subAttrName, json, publishTime, NULL, false);
}



// -----------------------------------------------------------------------------
//
// bridgeGoalWrite -
//
int bridgeGoalWrite(const char* bridgeName,
                    const char* endpoint,
                    const char* goalAlias,
                    const char* subAttrName,
                    const char* json,
                    int64_t     publishTime,
                    const char* requestJson)
{
  return sampleIn(bridgeName, endpoint, goalAlias, subAttrName, json, publishTime, requestJson, true);
}



// -----------------------------------------------------------------------------
//
// bridgeGoalInstanceRemove -
//
// ⭐ EXACTLY WHAT A CLIENT'S  DELETE .../attrs/{attr}?datasetId=<alias>  DOES,
// and not the instance-level null of a PATCH, although that would store the
// same thing. The two differ in what they SAY: a delete is reported as
// attributeDeleted naming the instance, so a watcher's notification carries the
// "urn:ngsi-ld:null" marker for it and the temporal history records a deletion,
// where the null would have been an update that emptied the notification and a
// replace in the history.
//
int bridgeGoalInstanceRemove(const char* bridgeName, const char* endpoint, const char* goalAlias)
{
  Channel* channelP = channelLookup(bridgeName, endpoint);

  if ((channelP == NULL) || (goalAlias == NULL))
    return BRIDGE_NOT_FOUND;

  if ((db.entityRetrieve == NULL) || (db.entityReplace == NULL))
    return BRIDGE_UNSUPPORTED;

  Tenant*     tenantP  = channelP->tenantP;
  const char* entityId = channelP->entityId;
  const char* attrName = channelP->attrName;

  threadBind(tenantP);

  KjNode* entityP = NULL;

  if ((db.entityRetrieve(tenantP, entityId, &entityP) != DB_OK) || (entityP == NULL))
    return BRIDGE_NOT_FOUND;

  KjNode* attrP     = kjLookup(entityP, attrName);
  KjNode* instanceP = (attrP != NULL) ? kjLookup(attrP, goalAlias) : NULL;

  if (instanceP == NULL)
    return BRIDGE_NOT_FOUND;

  KjNode* preSnapshotP = kjClone(corRest.kjsonP, attrP);

  kjChildRemove(attrP, instanceP);

  if (attrP->value.firstChildP == NULL)
    kjChildRemove(entityP, attrP);

  KjNode* oldEntityP = NULL;

  if (db.entityReplace(tenantP, entityId, entityP, &oldEntityP) != DB_OK)
  {
    KT_W("bridge '%s': the instance %s of %s/%s could not be removed", bridgeName, goalAlias, entityId, attrName);
    return BRIDGE_ERR;
  }

  if (tenantP->subCacheP != NULL)
  {
    LdMergeReport report;
    KjNode*       entryP  = kjObject(corRest.kjsonP, NULL);
    KjNode*       dsKeysP = kjArray(corRest.kjsonP, "datasetIds");

    report.changes = kjArray(corRest.kjsonP, "changes");

    kjChildAdd(dsKeysP, kjString(corRest.kjsonP, NULL, (char*) goalAlias));
    kjChildAdd(entryP, kjString(corRest.kjsonP, "attr",   (char*) attrName));
    kjChildAdd(entryP, kjString(corRest.kjsonP, "reason", "attributeDeleted"));
    kjChildAdd(entryP, dsKeysP);

    if (preSnapshotP != NULL)
    {
      KjNode* preValueP = kjClone(corRest.kjsonP, preSnapshotP);

      preValueP->name = (char*) "preValue";
      kjChildAdd(entryP, preValueP);
    }

    kjChildAdd(report.changes, entryP);
    ldNotifyDefer((LdSubCache*) tenantP->subCacheP, entityP, LdNotifyEntityUpdate, &report);
  }

  if (troe.attrEvent != NULL || troe.eventList != NULL)
  {
    TroeEvent* tevP = (TroeEvent*) kaAlloc(&corRest.kalloc, sizeof(TroeEvent));

    memset(tevP, 0, sizeof(TroeEvent));
    tevP->op             = TroeOpAttrDeleted;
    tevP->tenantP        = tenantP;
    tevP->entityId       = entityId;
    tevP->entityType     = channelP->entityType;
    tevP->attrName       = attrName;
    tevP->modifiedAtNs   = corRest.requestStartTime;
    tevP->entitySnapshot = entityP;
    tevP->attrSnapshot   = preSnapshotP;              // the pre-delete wrapper, as a client's delete gives
    troeDeferAttrEvent(tevP);
  }

  //
  // Drained here as sampleIn() drains: there is no request behind this.
  //
  ldNotifyDispatchPending();
  ldCsrSubDispatchPending();
  troeDispatchPending();
  ldSubEntityTypeExprsRelease();
  corNgsildFallbackRelease();                         // this thread's queues - see sampleIn()

  KT_T(KtBridge, "instance %s of %s/%s removed", goalAlias, entityId, attrName);

  return BRIDGE_OK;
}
