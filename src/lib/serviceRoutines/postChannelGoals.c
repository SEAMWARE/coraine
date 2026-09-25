//
// FILE            postChannelGoals.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /ngsi-ld/v1/channels/{channelId}/goals - send a goal on an action Channel
//
//   { "goalRequest": <the goal, the transport's JSON>, "endpoint": "<where its events go>" }
//
// The goal does to the entity exactly what a PATCH of its attribute does - DDS
// first, the goal's own instance (datasetId urn:goal:<id>), its notifications,
// its TRoE history - so that is what runs: patchEntityAttrsOn, with the
// fragment such a PATCH would carry.
//
// ⭐ Then it WAITS for the transport's answer (KZ: "just for creation, let's
// wait - we get much better error handling as well if we wait"):
//
//   accepted          -> 201, Location .../goals/<the transport's goal id>
//   rejected          -> 422, errorCode goalRejected - the server refused it,
//                        and the transport says no more than that
//   lost by transport -> 503, errorCode goalFailed
//   no answer in time -> 504, errorCode goalNotAnswered (--ddsSyncTimeout) -
//                        the goal WAS sent; it may run
//   not sent at all   -> what the PATCH answers: 503 / 422, nothing written
//
#include <stddef.h>                                   // NULL
#include <stdio.h>                                    // snprintf
#include <string.h>                                   // strlen

#include "kalloc/kaAlloc.h"                           // kaAlloc
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjObject, kjString, kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjClone.h"                            // kjClone
#include "kjson/kjParse.h"                            // kjParse
#include "kjson/kjRender.h"                           // kjFastRender
#include "kjson/kjRenderSize.h"                       // kjFastRenderSize
#include "corRest/CorRestState.h"                     // corRest
#include "corRest/corRestOutHeader.h"                 // corRestOutHeaderAdd
#include "corJsonld/corLdExpandTree.h"                // corLdExpandTree
#include "corNgsild/corNgsild.h"                      // corNgsild, ldError, ldContextResolve, LD_ERROR_*
#include "corNgsild/ldError.h"                        // ldErrorExtraString
#include "corBridge/BridgeBroker.h"                   // BridgeGoal*

#include "bridge/Channel.h"                           // Channel
#include "bridge/bridgeRender.h"                      // channelIdOf
#include "bridge/bridgeGoal.h"                        // bridgeGoalAwaitAnswer
#include "bridge/bridgeServiceSync.h"                 // bridgeSyncTimeoutMs
#include "serviceRoutines/patchEntityAttrs.h"         // patchEntityAttrsOn
#include "serviceRoutines/channelGoalCommon.h"        // actionChannelOfRequest
#include "serviceRoutines/postChannelGoals.h"         // Own interface



// -----------------------------------------------------------------------------
//
// goalFragment - the fragment a PATCH of the Channel's attribute with this goal would carry
//
// Built, rendered and parsed again, and EXPANDED as a request body is: a tree
// made by hand lacks the classification bits the expander puts on its nodes,
// and without them the NGSI-LD layer takes "type" and "value" for
// sub-Attributes.
//
static KjNode* goalFragment(Channel* channelP, KjNode* requestP, KjNode* endpointP)
{
  Kjson*  kjsonP = corRest.kjsonP;
  KjNode* fragP  = kjObject(kjsonP, NULL);
  KjNode* attrP  = kjObject(kjsonP, channelP->attrName);
  KjNode* valueP = kjClone(kjsonP, requestP);

  valueP->name = (char*) "value";

  kjChildAdd(attrP, kjString(kjsonP, "type", "Property"));
  kjChildAdd(attrP, valueP);

  if (endpointP != NULL)
  {
    KjNode* epP = kjObject(kjsonP, "endpoint");

    kjChildAdd(epP, kjString(kjsonP, "type",  "Property"));
    kjChildAdd(epP, kjString(kjsonP, "value", endpointP->value.s));
    kjChildAdd(attrP, epP);
  }

  kjChildAdd(fragP, attrP);

  char* text = (char*) kaAlloc(&corRest.kalloc, kjFastRenderSize(fragP) + 1);
  kjFastRender(fragP, text);

  KjNode* parsedP = kjParse(kjsonP, text);

  if (parsedP != NULL)
    corLdExpandTree(parsedP, corNgsild.contextP, &corRest.kalloc);

  return parsedP;
}



// -----------------------------------------------------------------------------
//
// postChannelGoals -
//
bool postChannelGoals(void)
{
  Channel* channelP = actionChannelOfRequest();

  if (channelP == NULL)
    return true;   // the error is set

  KjNode* bodyP     = corRest.in.requestTree;
  KjNode* requestP  = (bodyP != NULL) ? kjLookup(bodyP, "goalRequest") : NULL;
  KjNode* endpointP = (bodyP != NULL) ? kjLookup(bodyP, "endpoint")    : NULL;

  if ((bodyP == NULL) || (bodyP->type != KjObject) || (requestP == NULL))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Goal", "a goal needs a goalRequest - the goal, as the transport takes it");
    return true;
  }

  if ((endpointP != NULL) && (endpointP->type != KjString))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Goal", "endpoint: the URL the goal's events are notified to");
    return true;
  }

  ldContextResolve();

  KjNode* fragP = goalFragment(channelP, requestP, endpointP);

  if (fragP == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "the goal's fragment could not be built");
    return true;
  }

  uint64_t token = 0;

  patchEntityAttrsOn(channelP->entityId, fragP, &token);

  if (corRest.out.httpStatusCode >= 400)
    return true;   // not sent, or not written - the PATCH has said why

  if (token == 0)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "the goal was written but no goal was sent");
    return true;
  }

  int  state = 0;
  char goalId[128];

  if (bridgeGoalAwaitAnswer(token, bridgeSyncTimeoutMs, &state, goalId, sizeof(goalId)) == false)
  {
    ldError(504, LD_ERROR_INTERNAL_ERROR, "Goal Not Answered",
            "the goal was sent, and the transport did not answer within %d ms - it may run", bridgeSyncTimeoutMs);
    ldErrorExtraString("errorCode", "goalNotAnswered");
    return true;
  }

  if (state == BridgeGoalRejected)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Goal Rejected", "the server of channel '%s' rejected the goal", channelIdOf(channelP));
    ldErrorExtraString("errorCode", "goalRejected");
    return true;
  }

  if (state == BridgeGoalFailed)
  {
    ldError(503, LD_ERROR_INTERNAL_ERROR, "Goal Failed", "the transport lost the goal before it was accepted");
    ldErrorExtraString("errorCode", "goalFailed");
    return true;
  }

  const char* cId = channelIdOf(channelP);
  int         len = 21 + strlen(cId) + 7 + strlen(goalId) + 1;   // "/ngsi-ld/v1/channels/" + "/goals/"
  char*       loc = (char*) kaAlloc(&corRest.kalloc, len);

  snprintf(loc, len, "/ngsi-ld/v1/channels/%s/goals/%s", cId, goalId);

  corRest.out.httpStatusCode = 201;
  corRest.out.responseTree   = NULL;
  corRestOutHeaderAdd("Location", loc);

  return true;
}
