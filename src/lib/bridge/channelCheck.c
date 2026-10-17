//
// FILE            channelCheck.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf
#include <string.h>                                   // strcmp, strchr, strncat, strlen

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corTree/corTreeBuilder.h"                   // corTreeChildRemove
#include "corLog/corLog.h"                            // COR_T
#include "corNgsild/corNgsild.h"                      // corNgsild, ldError, LD_ERROR_*
#include "corNgsild/ldIsEntityKeyword.h"              // ldIsNotAttribute
#include "corBridge/BridgeDriver.h"                   // bridges, bridgeCount, BridgeDriver

#include "db/Tenant.h"                                // Tenant
#include "serviceRoutines/AttrsNotWritten.h"          // AttrsNotWritten, attrsNotWrittenAdd
#include "bridge/Channel.h"                           // Channel
#include "bridge/channelCache.h"                      // channelLookupByTarget
#include "bridge/bridgeServiceSync.h"                 // bridgeWireRender, BRIDGE_REQ_PER_ENTITY
#if COR_FEATURE_DATA_CHANNELS
#include "bridge/DataChannel.h"                       // DataChannel
#include "bridge/RecordMap.h"                         // RecordEntity, RecordAttr
#include "bridge/dataChannelCache.h"                  // dataChannelFirst
#endif
#if COR_FEATURE_ASSET_CHANNELS
#include "bridge/AssetChannel.h"                      // AssetChannel
#include "bridge/assetChannelCache.h"                 // assetChannelMappedLookup
#endif
#include "bridge/channelCheck.h"                      // Own interface
#include "coraineTraceLevels.h"                       // CtBridge



// -----------------------------------------------------------------------------
//
// checkingBridges - how many loaded bridges have a payload check; -1: not counted yet
//
// The bridges are loaded once, at startup, before the first request: counted on the first write, the
// count never changes. Two workers counting at once write the same number.
//
static int checkingBridges = -1;



// -----------------------------------------------------------------------------
//
// channelChecksActive -
//
bool channelChecksActive(void)
{
  int n = __atomic_load_n(&checkingBridges, __ATOMIC_RELAXED);

  if (n < 0)
  {
    n = 0;

    for (int ix = 0; ix < bridgeCount; ix++)
    {
      if (bridges[ix].payloadCheck != NULL)
        n++;
    }

    __atomic_store_n(&checkingBridges, n, __ATOMIC_RELAXED);
  }

  return n > 0;
}



// -----------------------------------------------------------------------------
//
// checkerOf - the loaded bridge 'alias', when it has a payload check
//
static BridgeDriver* checkerOf(const char* alias)
{
  for (int ix = 0; ix < bridgeCount; ix++)
  {
    if ((bridges[ix].alias != NULL) && (strcmp(bridges[ix].alias, alias) == 0))
      return (bridges[ix].payloadCheck != NULL) ? &bridges[ix] : NULL;
  }

  return NULL;
}



#if COR_FEATURE_DATA_CHANNELS
// -----------------------------------------------------------------------------
//
// idTemplateMatches - is 'id' one the id template 't' makes? "{Column}" stands for one or more characters
//
static bool idTemplateMatches(const char* t, const char* id)
{
  while (*t != 0)
  {
    if (*t == '{')
    {
      const char* closeP = strchr(t, '}');

      if (closeP == NULL)
        return strcmp(t, id) == 0;                    // no placeholder after all: the rest, literally

      t = closeP + 1;

      if (*t == 0)
        return *id != 0;                              // the placeholder is the rest of the id

      for (const char* restP = id + 1; *restP != 0; restP++)
      {
        if (idTemplateMatches(t, restP) == true)
          return true;
      }

      return false;
    }

    if (*t != *id)
      return false;

    t++;
    id++;
  }

  return *id == 0;
}



// -----------------------------------------------------------------------------
//
// dataChannelOf - the DataChannel of a checking bridge whose mapping makes this attribute of this entity
//
// A DataChannel names no entity: it maps the entities its id templates make. The entity is one of its
// when its id fits one of its templates, and the attribute is one of its when that template's entity
// maps it. The first such DataChannel answers.
//
static DataChannel* dataChannelOf(Tenant* tenantP, const char* entityId, const char* attrName, RecordAttr** recordAttrPP)
{
  for (DataChannel* dcP = dataChannelFirst(); dcP != NULL; dcP = dcP->next)
  {
    if ((dcP->tenantP != tenantP) || (checkerOf(dcP->bridgeName) == NULL))
      continue;

    for (RecordEntity* entityP = dcP->mapP->entities; entityP != NULL; entityP = entityP->next)
    {
      if (idTemplateMatches(entityP->id, entityId) == false)
        continue;

      for (RecordAttr* attrP = entityP->attrs; attrP != NULL; attrP = attrP->next)
      {
        if (strcmp(attrP->name, attrName) == 0)
        {
          *recordAttrPP = attrP;
          return dcP;
        }
      }
    }
  }

  return NULL;
}
#endif



// -----------------------------------------------------------------------------
//
// channelChecksBeforeWrite -
//
bool channelChecksBeforeWrite(Tenant* tenantP, const char* entityId, CorNode* fragmentP, int flags, AttrsNotWritten* notWrittenP)
{
  if ((channelChecksActive() == false) || (entityId == NULL) || (fragmentP == NULL))
    return true;

  if (corNgsild.bridgeOwnWrite == true)               // a DataChannel's record: the bridge is not checked against itself
    return true;

  //
  // Several attributes, or a batch: a refused one is left out, and the rest goes on - as for a value the
  // send step could not send (bridgeRequestsBeforeWrite)
  //
  int attrCount = 0;

  for (CorNode* attrP = fragmentP->value.head; attrP != NULL; attrP = attrP->next)
  {
    if (ldIsNotAttribute(attrP) == false)
      attrCount++;
  }

  bool several = (attrCount > 1) || ((flags & BRIDGE_REQ_PER_ENTITY) != 0);

  CorNode* nextP;

  for (CorNode* attrP = fragmentP->value.head; attrP != NULL; attrP = nextP)
  {
    nextP = attrP->next;                              // attrP may be taken out of the fragment

    if (ldIsNotAttribute(attrP) == true)
      continue;

    const char* bridgeName = NULL;
    const char* endpoint   = NULL;
    const char* attrName   = NULL;                    // the channel's - EXPANDED
    uint64_t*   refusedP   = NULL;
    Channel*    channelP   = channelLookupByTarget(tenantP, entityId, attrP->name);

    if (channelP != NULL)
    {
      bridgeName = channelP->bridgeName;
      endpoint   = channelP->endpoint;
      attrName   = channelP->attrName;
      refusedP   = &channelP->payloadsRefused;
    }
#if COR_FEATURE_DATA_CHANNELS
    else
    {
      RecordAttr*  recordAttrP = NULL;
      DataChannel* dcP         = dataChannelOf(tenantP, entityId, attrP->name, &recordAttrP);

      if (dcP != NULL)
      {
        bridgeName = dcP->bridgeName;
        endpoint   = dcP->endpoint;
        attrName   = recordAttrP->name;
        refusedP   = &dcP->payloadsRefused;
      }
    }
#endif
#if COR_FEATURE_ASSET_CHANNELS
    //
    // An AssetChannel's: an attribute its last model made. The endpoint the bridge is handed is the node the
    // attribute was made from - where the value lives at the source - or, for one made of no one node (a
    // container's, a Relationship), the model's root
    //
    if (bridgeName == NULL)
    {
      AssetChannel* acP    = NULL;
      const char*   nodeId = NULL;
      const char*   name   = NULL;

      if ((assetChannelMappedLookup(tenantP, entityId, attrP->name, &acP, &nodeId, &name) == true) && (checkerOf(acP->bridgeName) != NULL))
      {
        bridgeName = acP->bridgeName;
        endpoint   = (nodeId != NULL) ? nodeId : acP->endpoint;
        attrName   = name;
        refusedP   = &acP->payloadsRefused;
      }
    }
#endif

    if (bridgeName == NULL)
      continue;

    BridgeDriver* driverP = checkerOf(bridgeName);

    if (driverP == NULL)
      continue;

    //
    // The default instance's value - what a channel carries. An attribute without one (a datasetId
    // instance alone, a deletion) has nothing to check.
    //
    CorNode* instanceP = corTreeLookup(attrP, "@none");
    CorNode* valueP    = (instanceP != NULL) ? corTreeLookup(instanceP, "value") : NULL;

    if (valueP == NULL)
      continue;

    char* json = bridgeWireRender(valueP);

    if (json == NULL)
    {
      ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "out of memory rendering the value of '%s'", attrP->name);
      return false;
    }

    char reason[512];

    reason[0] = 0;

    int r = driverP->payloadCheck(endpoint, attrName, json, reason, sizeof(reason));

    if (r == BRIDGE_OK)
      continue;

    //
    // A refusal (BRIDGE_BAD_INPUT): the value is wrong - 400. Anything else: the check failed, and a value
    // nobody could check is not written either - 503, the bridge could not check it.
    //
    bool        refused = (r == BRIDGE_BAD_INPUT);
    int         status  = refused ? 400 : 503;
    const char* type    = refused ? LD_ERROR_BAD_REQUEST_DATA : LD_ERROR_INTERNAL_ERROR;
    const char* title   = refused ? "Invalid request" : "Service Unavailable";
    const char* why     = (reason[0] != 0) ? reason : "no reason given";

    __atomic_add_fetch(refusedP, 1, __ATOMIC_RELAXED);
    COR_T(CtBridge, "%s/%s: the payload check of bridge '%s' ('%s') %s: %s", entityId, attrP->name, bridgeName, endpoint, refused ? "refused it" : "failed", why);

    char detail[1024];

    if (refused == true)
      snprintf(detail, sizeof(detail), "bridge '%s' refused the value of '%s' (%s): %s", bridgeName, attrP->name, endpoint, why);
    else
      snprintf(detail, sizeof(detail), "bridge '%s' could not check the value of '%s' (%s): %s", bridgeName, attrP->name, endpoint, why);

    if (several == false)
    {
      ldError(status, type, title, "%s - nothing was written", detail);
      return false;
    }

    strncat(detail, " - not written", sizeof(detail) - strlen(detail) - 1);
    attrsNotWrittenAdd(notWrittenP, attrP->name, status, type, title, detail);
    corTreeChildRemove(fragmentP, attrP);
  }

  return true;
}
