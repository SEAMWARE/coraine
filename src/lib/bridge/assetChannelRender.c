//
// FILE            assetChannelRender.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// An AssetChannel for GET /ngsi-ld/v1/assetChannels[/{id}]: its id, its Bridge, its endpoint (the model's
// root), the mapping as the file wrote it, its status, and what it has written.
//
#include <stdio.h>                                    // snprintf
#include <string.h>                                   // strlen, memcpy

#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeString, corTreeInteger, corTreeChildAdd
#include "corRest/CorRestState.h"                     // corRest

#include "bridge/AssetChannel.h"                      // AssetChannel
#include "bridge/bridgeRender.h"                      // bridgeIdOf
#include "bridge/assetChannelRender.h"                // Own interface



// -----------------------------------------------------------------------------
//
// assetChannelIdOf -
//
const char* assetChannelIdOf(AssetChannel* acP)
{
  int   len = 25 + strlen(acP->bridgeName) + 1 + strlen(acP->endpoint) + 1;   // "urn:ngsi-ld:AssetChannel:" = 25
  char* id  = (char*) corAlloc(&corRest.kalloc, len);

  if (id != NULL)
    snprintf(id, len, "urn:ngsi-ld:AssetChannel:%s:%s", acP->bridgeName, acP->endpoint);

  return id;
}



// -----------------------------------------------------------------------------
//
// jsonMember - a JSON text, parsed back (a copy: the parser writes into its buffer), as member 'name'
//
static void jsonMember(CorNode* bodyP, const char* name, const char* text)
{
  if ((text == NULL) || (corRest.corJsonP == NULL))
    return;

  int   len  = strlen(text);
  char* copy = corAlloc(corRest.kallocP, len + 1);

  if (copy == NULL)
    return;

  memcpy(copy, text, len + 1);

  CorNode* nodeP = corJsonParse(corRest.corJsonP, copy);

  if (nodeP != NULL)
  {
    nodeP->name = (char*) name;
    corTreeChildAdd(bodyP, nodeP);
  }
}



// -----------------------------------------------------------------------------
//
// layoutName -
//
static const char* layoutName(AssetLayout layout)
{
  switch (layout)
  {
  case AssetLayoutAttributes:    return "attributes";
  case AssetLayoutSubAttributes: return "subAttributes";
  case AssetLayoutValue:         return "value";
  }

  return "attributes";
}



// -----------------------------------------------------------------------------
//
// assetChannelRender -
//
CorNode* assetChannelRender(AssetChannel* acP)
{
  CorAlloc* allocP = corRest.kallocP;
  CorNode*  bodyP  = corTreeObject(allocP, NULL);

  corTreeChildAdd(bodyP, corTreeString(allocP, "id",            (char*) assetChannelIdOf(acP)));
  corTreeChildAdd(bodyP, corTreeString(allocP, "type",          "AssetChannel"));
  corTreeChildAdd(bodyP, corTreeString(allocP, "bridgeId",      (char*) bridgeIdOf(acP->bridgeName)));
  corTreeChildAdd(bodyP, corTreeString(allocP, "channelTarget", acP->endpoint));

  jsonMember(bodyP, "channelInfo", acP->info);

  jsonMember(bodyP, "assetEntities", acP->mapP->text);
  corTreeChildAdd(bodyP, corTreeString(allocP, "attributeLayout", (char*) layoutName(acP->mapP->layout)));

  ChannelStatus status = __atomic_load_n(&acP->status, __ATOMIC_ACQUIRE);           // assetChannelStatusSet, on a plugin thread
  char*         reason = __atomic_load_n(&acP->statusReason, __ATOMIC_ACQUIRE);

  corTreeChildAdd(bodyP, corTreeString(allocP, "status", (status == ChannelStatusAvailable) ? "available" : "dormant"));

  if (reason != NULL)
    corTreeChildAdd(bodyP, corTreeString(allocP, "statusReason", reason));

  corTreeChildAdd(bodyP, corTreeInteger(allocP, "modelsIn",        (long long) __atomic_load_n(&acP->modelsIn,        __ATOMIC_RELAXED)));
  corTreeChildAdd(bodyP, corTreeInteger(allocP, "valuesIn",        (long long) __atomic_load_n(&acP->valuesIn,        __ATOMIC_RELAXED)));
  corTreeChildAdd(bodyP, corTreeInteger(allocP, "valuesRefused",   (long long) __atomic_load_n(&acP->valuesRefused,   __ATOMIC_RELAXED)));
  corTreeChildAdd(bodyP, corTreeInteger(allocP, "samplesRefused",  (long long) __atomic_load_n(&acP->samplesRefused,  __ATOMIC_RELAXED)));
  corTreeChildAdd(bodyP, corTreeInteger(allocP, "entitiesWritten", (long long) __atomic_load_n(&acP->entitiesWritten, __ATOMIC_RELAXED)));
#if COR_FEATURE_CHANNEL_CHECKS
  corTreeChildAdd(bodyP, corTreeInteger(allocP, "payloadsRefused", (long long) __atomic_load_n(&acP->payloadsRefused, __ATOMIC_RELAXED)));
#endif

  return bodyP;
}
