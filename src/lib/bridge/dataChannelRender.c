//
// FILE            dataChannelRender.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A DataChannel for GET /ngsi-ld/v1/dataChannels[/{id}]: its id, its Bridge, its endpoint, the mapping as
// the file wrote it, its status, and what it has written.
//
#include <stdio.h>                                    // snprintf
#include <string.h>                                   // strlen, memcpy

#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeString, corTreeInteger, corTreeChildAdd
#include "corRest/CorRestState.h"                     // corRest

#include "bridge/DataChannel.h"                       // DataChannel
#include "bridge/bridgeRender.h"                      // bridgeIdOf
#include "bridge/dataChannelRender.h"                 // Own interface



// -----------------------------------------------------------------------------
//
// dataChannelIdOf -
//
const char* dataChannelIdOf(DataChannel* dcP)
{
  int   len = 24 + strlen(dcP->bridgeName) + 1 + strlen(dcP->endpoint) + 1;   // "urn:ngsi-ld:DataChannel:" = 24
  char* id  = (char*) corAlloc(&corRest.kalloc, len);

  if (id != NULL)
    snprintf(id, len, "urn:ngsi-ld:DataChannel:%s:%s", dcP->bridgeName, dcP->endpoint);

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
// dataChannelRender -
//
CorNode* dataChannelRender(DataChannel* dcP)
{
  CorAlloc* allocP = corRest.kallocP;
  CorNode*  bodyP  = corTreeObject(allocP, NULL);

  corTreeChildAdd(bodyP, corTreeString(allocP, "id",            (char*) dataChannelIdOf(dcP)));
  corTreeChildAdd(bodyP, corTreeString(allocP, "type",          "DataChannel"));
  corTreeChildAdd(bodyP, corTreeString(allocP, "bridgeId",      (char*) bridgeIdOf(dcP->bridgeName)));
  corTreeChildAdd(bodyP, corTreeString(allocP, "channelTarget", dcP->endpoint));

  jsonMember(bodyP, "channelInfo", dcP->info);

  corTreeChildAdd(bodyP, corTreeString(allocP, "idEncoding", (dcP->mapP->idEncoding == RecordIdPercent) ? "percent" : "merge"));
  jsonMember(bodyP, "recordEntities", dcP->mapP->text);

  ChannelStatus status = __atomic_load_n(&dcP->status, __ATOMIC_ACQUIRE);           // dataChannelStatusSet, on a plugin thread
  char*         reason = __atomic_load_n(&dcP->statusReason, __ATOMIC_ACQUIRE);

  corTreeChildAdd(bodyP, corTreeString(allocP, "status", (status == ChannelStatusAvailable) ? "available" : "dormant"));

  if (reason != NULL)
    corTreeChildAdd(bodyP, corTreeString(allocP, "statusReason", reason));

  corTreeChildAdd(bodyP, corTreeInteger(allocP, "recordsIn",       (long long) __atomic_load_n(&dcP->recordsIn,       __ATOMIC_RELAXED)));
  corTreeChildAdd(bodyP, corTreeInteger(allocP, "recordsRefused",  (long long) __atomic_load_n(&dcP->recordsRefused,  __ATOMIC_RELAXED)));
  corTreeChildAdd(bodyP, corTreeInteger(allocP, "entitiesWritten", (long long) __atomic_load_n(&dcP->entitiesWritten, __ATOMIC_RELAXED)));
#if COR_FEATURE_CHANNEL_CHECKS
  corTreeChildAdd(bodyP, corTreeInteger(allocP, "payloadsRefused", (long long) __atomic_load_n(&dcP->payloadsRefused, __ATOMIC_RELAXED)));
#endif
#if COR_FEATURE_CHANNEL_WRITE_BACK
  if (dcP->direction == BridgeDirectionBoth)          // only then: "in" is the default, and what every DataChannel was
  {
    corTreeChildAdd(bodyP, corTreeString(allocP,  "channelDirection", "both"));
    corTreeChildAdd(bodyP, corTreeInteger(allocP, "recordsOut", (long long) __atomic_load_n(&dcP->recordsOut, __ATOMIC_RELAXED)));
  }
#endif

  return bodyP;
}
