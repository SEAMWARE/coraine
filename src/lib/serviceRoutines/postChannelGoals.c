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
// The goal does to the entity exactly what a PATCH of its attribute does - the
// transport deciding first, the goal's own instance (datasetId urn:goal:<id>),
// its notifications, its TRoE history - so that is what runs: patchEntityAttrsOn,
// with the fragment such a PATCH would carry.
//
// And that PATCH already WAITS for the transport's answer, as every write to an
// action Channel does, and writes nothing unless the goal was accepted (KZ:
// "just for creation, let's wait - we get much better error handling as well"):
//
//   accepted          -> 201, Location .../goals/<the transport's goal id>
//   rejected          -> 422, errorCode goalRejected - the server refused it,
//                        and the transport says no more than that
//   lost by transport -> 503, errorCode goalFailed
//   no answer in time -> 504, errorCode goalNotAnswered (--ddsSyncTimeout) -
//                        and the goal is cancelled
//   not sent at all   -> what the PATCH answers: 503 / 422
//
// Nothing is written in any of the failures.
//
#include <stddef.h>                                   // NULL
#include <stdio.h>                                    // snprintf
#include <string.h>                                   // strlen

#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeString, corTreeChildAdd
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corJson/corJsonRender.h"                    // corJsonFastRender
#include "corJson/corJsonRenderSize.h"                // corJsonFastRenderSize
#include "corRest/CorRestState.h"                     // corRest
#include "corRest/corRestOutHeader.h"                 // corRestOutHeaderAdd
#include "corJsonld/corLdExpandTree.h"                // corLdExpandTree
#include "corNgsild/corNgsild.h"                      // corNgsild, ldError, ldContextResolve, LD_ERROR_*

#include "bridge/Channel.h"                           // Channel
#include "bridge/bridgeRender.h"                      // channelIdOf
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
static CorNode* goalFragment(Channel* channelP, CorNode* requestP, CorNode* endpointP)
{
  CorJson* corJsonP = corRest.corJsonP;
  CorNode* fragP = corTreeObject(corRest.kallocP, NULL);
  CorNode* attrP = corTreeObject(corRest.kallocP, channelP->attrName);
  CorNode* valueP = corTreeClone(corRest.kallocP, requestP);

  valueP->name = (char*) "value";

  corTreeChildAdd(attrP, corTreeString(corRest.kallocP, "type", "Property"));
  corTreeChildAdd(attrP, valueP);

  if (endpointP != NULL)
  {
    CorNode* epP = corTreeObject(corRest.kallocP, "endpoint");

    corTreeChildAdd(epP, corTreeString(corRest.kallocP, "type", "Property"));
    corTreeChildAdd(epP, corTreeString(corRest.kallocP, "value", endpointP->value.s));
    corTreeChildAdd(attrP, epP);
  }

  corTreeChildAdd(fragP, attrP);

  char* text = (char*) corAlloc(&corRest.kalloc, corJsonFastRenderSize(fragP) + 1);
  corJsonFastRender(fragP, text);

  CorNode* parsedP = corJsonParse(corJsonP, text);

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

  CorNode* bodyP    = corRest.in.requestTree;
  CorNode* requestP = (bodyP != NULL) ? corTreeLookup(bodyP, "goalRequest") : NULL;
  CorNode* endpointP = (bodyP != NULL) ? corTreeLookup(bodyP, "endpoint") : NULL;

  if ((bodyP == NULL) || (bodyP->type != CorObject) || (requestP == NULL))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Goal", "a goal needs a goalRequest - the goal, as the transport takes it");
    return true;
  }

  if ((endpointP != NULL) && (endpointP->type != CorString))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Goal", "endpoint: the URL the goal's events are notified to");
    return true;
  }

  ldContextResolve();

  CorNode* fragP = goalFragment(channelP, requestP, endpointP);

  if (fragP == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "the goal's fragment could not be built");
    return true;
  }

  //
  // The PATCH waits for the transport to decide, as every write to an action
  // Channel does, and writes nothing for a goal that was not accepted - its
  // error is this POST's: 422 goalRejected, 503 goalFailed, 504 goalNotAnswered.
  //
  char* goalId = NULL;

  patchEntityAttrsOn(channelP->entityId, fragP, &goalId);

  if (corRest.out.httpStatusCode >= 400)
    return true;   // not taken on, or not written - the PATCH has said why

  if (goalId == NULL)
  {
    ldError(502, LD_ERROR_INTERNAL_ERROR, "Bad Gateway", "the transport accepted the goal without giving it an id");
    return true;
  }

  const char* cId = channelIdOf(channelP);
  int         len = 21 + strlen(cId) + 7 + strlen(goalId) + 1;   // "/ngsi-ld/v1/channels/" + "/goals/"
  char*       loc = (char*) corAlloc(&corRest.kalloc, len);

  snprintf(loc, len, "/ngsi-ld/v1/channels/%s/goals/%s", cId, goalId);

  corRest.out.httpStatusCode = 201;
  corRest.out.responseTree   = NULL;
  corRestOutHeaderAdd("Location", loc);

  return true;
}
