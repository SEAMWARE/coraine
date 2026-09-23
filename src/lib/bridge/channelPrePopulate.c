//
// FILE            channelPrePopulate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <string.h>                                   // strcmp, memset

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjObject, kjString, kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup
#include "ktrace/kTrace.h"                            // KT_W, KT_T
#include "kalloc/kaAlloc.h"                           // kaAlloc

#include "corRest/corRest.h"                          // corRest
#include "corNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel

#include "db/DbDriver.h"                              // db, DB_OK, DB_NOT_FOUND
#include "troe/TroeDriver.h"                          // troe, TroeEvent, TroeOpEntityCreated
#include "troe/troeDispatch.h"                        // troeDeferEntityEvent, troeDispatchPending
#include "bridge/Channel.h"                           // Channel
#include "bridge/channelCache.h"                      // channelCacheFirst
#include "bridge/channelPrePopulate.h"                // Own interface
#include "coraineTraceLevels.h"                       // KtBridge



// -----------------------------------------------------------------------------
//
// CHANNEL_PLACEHOLDER_VALUE - what an attribute holds before anything publishes
//
// A string, and a self-describing one. Something has to be stored so the
// attribute exists and can be queried, subscribed to and shown, and every
// choice is wrong in some reading: 0 is a plausible measurement, null asks
// every consumer to handle it, and absent is what this exists to avoid. A word
// that is obviously not a reading is the one that misleads nobody.
//
#define CHANNEL_PLACEHOLDER_VALUE "uninitialized"



// -----------------------------------------------------------------------------
//
// placeholderAttribute - { "type": "Property", "value": "uninitialized" }
//
static KjNode* placeholderAttribute(const char* attrName)
{
  KjNode* attrP = kjObject(corRest.kjsonP, attrName);

  kjChildAdd(attrP, kjString(corRest.kjsonP, "type",  "Property"));
  kjChildAdd(attrP, kjString(corRest.kjsonP, "value", CHANNEL_PLACEHOLDER_VALUE));

  return attrP;
}



// -----------------------------------------------------------------------------
//
// entitySeenEarlier - has a Channel before this one already named this entity?
//
// Several Channels commonly share an entity - a robot publishing pose, speed
// and battery on three topics is three Channels and one entity - and it must be
// handled once, with all three attributes, not three times.
//
// A walk from the head rather than a set: the list is as long as the
// configuration file, this runs once at startup, and a hash whose only purpose
// is to make a short walk shorter is a hash to keep in step for nothing.
//
static bool entitySeenEarlier(Channel* upToP, Tenant* tenantP, const char* entityId)
{
  for (Channel* channelP = channelCacheFirst(); channelP != upToP; channelP = channelP->next)
  {
    if ((channelP->tenantP == tenantP) && (strcmp(channelP->entityId, entityId) == 0))
      return true;
  }

  return false;
}



  int attrsCreated = 0;
// -----------------------------------------------------------------------------
//
// channelPrePopulate -
//
int channelPrePopulate(Tenant* tenantP)
{
  if ((db.entityRetrieve == NULL) || (db.entityCreate == NULL) || (db.entityAttrsSet == NULL))
  {
    KT_W("the database plugin cannot pre-populate - channels will write into whatever is already there");
    return -1;
  }

  int attrsCreated = 0;

  for (Channel* channelP = channelCacheFirst(); channelP != NULL; channelP = channelP->next)
  {
    if (channelP->tenantP != tenantP)
      continue;

    if (entitySeenEarlier(channelP, tenantP, channelP->entityId) == true)
      continue;

    const char* entityId = channelP->entityId;

    KjNode* existingP = NULL;
    int     r         = db.entityRetrieve(tenantP, entityId, &existingP);

    if ((r != DB_OK) && (r != DB_NOT_FOUND))
    {
      KT_W("cannot read entity '%s' - not pre-populating it", entityId);
      continue;
    }

    bool exists = ((r == DB_OK) && (existingP != NULL));

    //
    // Built straight into its final object, never into a temporary that is
    // moved across afterwards. kjChildAdd RE-LINKS the node it is given - it
    // sets its next to NULL - so walking a list while adding from it stops
    // dead after the first element, silently and with the right count already
    // logged. There is no kjChildMove.
    //
    // A create and an update need exactly the same tree here, so one is built
    // and the choice of which driver call takes it is made afterwards.
    //
    KjNode* entityP = kjObject(corRest.kjsonP, NULL);
    int     missing = 0;

    //
    // id and type belong to a whole entity, not to a fragment. entityAttrsSet
    // takes the attributes alone - an 'id' in a fragment becomes '_id' in the
    // DB model, and the update then silently writes nothing while answering
    // DB_OK, which is how this was found.
    //
    if (exists == false)
    {
      kjChildAdd(entityP, kjString(corRest.kjsonP, "id",   entityId));
      kjChildAdd(entityP, kjString(corRest.kjsonP, "type", channelP->entityType));
    }

    //
    // Every Channel of this entity, this one included, in one pass.
    //
    for (Channel* attrChannelP = channelP; attrChannelP != NULL; attrChannelP = attrChannelP->next)
    {
      if (attrChannelP->tenantP != tenantP)
        continue;

      if (strcmp(attrChannelP->entityId, entityId) != 0)
        continue;

      //
      // ⭐ Present already: leave it alone. This runs on every start, and an
      // attribute that has been carrying real values for a month is not to be
      // reset to a placeholder because the broker restarted.
      //
      if ((exists == true) && (kjLookup(existingP, attrChannelP->attrName) != NULL))
        continue;

      if (strcmp(attrChannelP->entityType, channelP->entityType) != 0)
        KT_W("entity '%s' is declared as both '%s' and '%s' in the configuration - using the first",
             entityId, channelP->entityType, attrChannelP->entityType);

      kjChildAdd(entityP, placeholderAttribute(attrChannelP->attrName));
      ++missing;
    }

    if (missing == 0)
      continue;

    //
    // 0 means "this is a create, stamp now" - a non-zero value is for
    // preserving a stored createdAt across a Replace, which this never is.
    //
    ldApiEntityToDbModel(entityP, &corRest.kalloc, 0);

    //
    // No notification either way. Nothing has been observed - this is the
    // broker writing down what its own configuration says it is about to be
    // told, and a subscriber woken by "uninitialized" learns only that a
    // broker restarted.
    //
    if (exists == false)
    {
      if (db.entityCreate(tenantP, entityId, entityP) != DB_OK)
      {
        KT_W("could not create entity '%s'", entityId);
        continue;
      }

      //
      // ⭐ AND IT GOES INTO HISTORY, which is not the same decision as the
      // notification two comments above.
      //
      // Not notifying is right: a subscriber woken by "uninitialized" learns
      // only that a broker restarted. History is the opposite case - the
      // entity genuinely came into existence at this moment, and the temporal
      // read needs to know it. Without this row a DDS-fed entity has attribute
      // history and no entity row, and that is not "a timeline missing its
      // first entry": the retrieve resolves an entity's TYPE from
      // troe_entities, so every temporal query against it answers 404 while
      // the samples pile up in troe_attrs unread.
      //
      if (troe.entityEvent != NULL || troe.eventList != NULL)
      {
        TroeEvent* tevP = (TroeEvent*) kaAlloc(&corRest.kalloc, sizeof(TroeEvent));

        memset(tevP, 0, sizeof(TroeEvent));
        tevP->op             = TroeOpEntityCreated;
        tevP->tenantP        = tenantP;
        tevP->entityId       = entityId;
        tevP->entityType     = channelP->entityType;
        tevP->modifiedAtNs   = corRest.requestStartTime;
        tevP->entitySnapshot = entityP;
        troeDeferEntityEvent(tevP);
      }

      KT_T(KtBridge, "pre-populated entity '%s' (%d attribute%s)", entityId, missing, (missing == 1) ? "" : "s");
    }
    else
    {
      //
      // ⚠ The report is NOT optional, whatever DbDriver.h says about passing
      // NULL to skip reporting. The mongoc driver builds its entire $set
      // document by walking reportP->changes, so a NULL report writes nothing
      // at all - and answers DB_OK while doing it. corDB has no such problem,
      // because there the merged tree IS the store. The two drivers disagree,
      // so the caller supplies one and ignores it.
      //
      LdMergeReport report = { NULL };

      if (db.entityAttrsSet(tenantP, entityId, entityP, false, corRest.requestStartTime, &report) != DB_OK)
      {
        KT_W("could not add the missing attributes of entity '%s'", entityId);
        continue;
      }

      KT_T(KtBridge, "entity '%s' gained %d missing attribute%s", entityId, missing, (missing == 1) ? "" : "s");
    }

    attrsCreated += missing;
  }

  //
  // ⭐⭐ AND DRAIN, because nothing else will.
  //
  // TRoE events are queued and emptied by the hook that runs once an HTTP
  // response has been sent. This runs at STARTUP: there is no request behind
  // it and no response ahead of it, so without this the events sit on the
  // queue until the first client request happens to flush them - or, on a
  // broker nobody talks to, never. Exactly the reason bridgeSampleIn drains
  // its own, and for the same reason: a write with no request behind it.
  //
  troeDispatchPending();

  return attrsCreated;
}
