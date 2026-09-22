//
// FILE            bridgeAttrOut.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjRender.h"                           // kjFastRender
#include "ktrace/kTrace.h"                            // KT_T, KT_W

#include "db/DbDriver.h"                              // db, DB_OK

#include "corBridge/BridgeDriver.h"                   // BridgeDriver, bridges, bridgeCount

#include "bridge/Channel.h"                           // Channel
#include "bridge/channelCache.h"                      // channelLookupByTarget, channelCount
#include "bridge/bridgeAttrOut.h"                     // Own interface
#include "coraineTraceLevels.h"                       // KtBridge



// -----------------------------------------------------------------------------
//
// BRIDGE_OUT_MAX - the largest payload put on a wire from here
//
// kjFastRender writes into a caller's buffer and does not know how big it is,
// so the size is decided here rather than discovered. A value larger than this
// is refused and said so, which is the one outcome that is neither silent nor
// a stack overrun.
//
#define BRIDGE_OUT_MAX (64 * 1024)



// -----------------------------------------------------------------------------
//
// bridgeAttrOut -
//
void bridgeAttrOut(Tenant* tenantP, const char* entityId, const char* attrName, KjNode* entityP)
{
  //
  // The common case is no bridges at all, and it must cost nothing: two
  // integer loads before anything else is touched.
  //
  if ((bridgeCount == 0) || (channelCount() == 0))
    return;

  if ((entityId == NULL) || (attrName == NULL))
    return;

  Channel* channelP = channelLookupByTarget(tenantP, entityId, attrName);

  if (channelP == NULL)
    return;

  if (channelP->direction == BridgeDirectionIn)
    return;                                            // inbound only - the broker listens, it does not answer

  if (channelP->status != ChannelStatusAvailable)
    return;                                            // its bridge is not loaded; the Channel is dormant

  //
  // Only now, with a Channel known to want it, is the entity worth having.
  //
  if (entityP == NULL)
  {
    if (db.entityRetrieve == NULL)
      return;

    if (db.entityRetrieve(tenantP, entityId, &entityP) != DB_OK)
      return;
  }

  KjNode* attrP = kjLookup(entityP, attrName);

  if (attrP == NULL)
    return;

  //
  // An attribute is stored dataset-keyed - "attr": { "@none": { type, value } }
  // - and callers hand over trees on either side of that conversion. Both are
  // accepted rather than one being declared correct, because which one a write
  // path happens to have is an accident of where the hook sits, and a mismatch
  // shows up only as silence.
  //
  // The default instance is the one that goes out. A datasetId names a
  // particular reading among several, and which of those a topic should carry
  // is not a question a configuration file with three fields per entry can
  // answer.
  //
  KjNode* instanceP = kjLookup(attrP, "@none");

  if (instanceP != NULL)
    attrP = instanceP;

  //
  // The wire carries the VALUE, not the NGSI-LD wrapper. A publisher on the
  // other end of a topic expects the application's own payload back, in the
  // shape it publishes - "type": "Property" is the broker's vocabulary and
  // means nothing on that wire.
  //
  KjNode* valueP = kjLookup(attrP, "value");

  if (valueP == NULL)
  {
    KT_T(KtBridge, "attribute '%s' of '%s' has no value to send", attrName, entityId);
    return;
  }

  static __thread char buf[BRIDGE_OUT_MAX];

  //
  // Rendering ONE node out of a tree needs it detached from its surroundings in
  // two ways, and each was found the hard way:
  //
  //   its NAME, or the output is  "value":250  instead of  250
  //   its NEXT, or the output is  250,  - kjFastRender follows the sibling
  //                               chain and emits the separator
  //
  // A trailing comma parses as nothing at all, so the round trip came back as
  // "not valid JSON" with no clue as to which end was wrong.
  //
  // Safe to touch: both callers hand over a request-local tree - the DB driver
  // returns a kjClone, not a pointer into its store - so nothing else is
  // looking at this node while the name and link are off.
  //
  char*   savedName = valueP->name;
  KjNode* savedNext = valueP->next;

  valueP->name = NULL;
  valueP->next = NULL;
  kjFastRender(valueP, buf);
  valueP->name = savedName;
  valueP->next = savedNext;

  for (int i = 0; i < bridgeCount; i++)
  {
    if ((bridges[i].alias == NULL) || (strcmp(bridges[i].alias, channelP->bridgeName) != 0))
      continue;

    if (bridges[i].publish == NULL)
      return;                                          // this transport does not send

    int r = bridges[i].publish(channelP->endpoint, buf);

    if (r != BRIDGE_OK)
      KT_W("bridge '%s' could not publish to '%s' (%d)", channelP->bridgeName, channelP->endpoint, r);
    else
      KT_T(KtBridge, "%s/%s -> '%s' on bridge '%s'", entityId, attrName, channelP->endpoint, channelP->bridgeName);

    return;
  }
}
