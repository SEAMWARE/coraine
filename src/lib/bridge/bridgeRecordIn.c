//
// FILE            bridgeRecordIn.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A record Channel's sample -> the entities its mapping makes -> one batch upsert. See bridgeRecordIn.h.
//
// The write goes through the service routine of POST /entityOperations/upsert, in this process, with
// no request around it - as coraine-import brings in subscriptions and registrations
// (migrateApiObject.c). An entity from a record is then checked, stored, notified and recorded in
// the temporal store exactly as one a client sends, and nothing here knows how.
//
#include <ctype.h>                                    // isspace
#include <errno.h>                                    // errno
#include <math.h>                                     // isfinite
#include <stdbool.h>                                  // bool
#include <stdlib.h>                                   // strtoll, strtod
#include <string.h>                                   // strcmp, strcasecmp, strlen
#include <strings.h>                                  // strcasecmp

#include "corAlloc/corAllocStrdup.h"                  // corAllocStrdup
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeArray, corTreeString, corTreeChildAdd
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corLog/corLog.h"                            // COR_W, COR_T
#include "corBase/corTimeIso.h"                       // corTimeIso

#include "corRest/corRest.h"                          // corRest
#include "corBridge/BridgeBroker.h"                   // BRIDGE_OK, BRIDGE_BAD_INPUT, BRIDGE_ERR
#include "corJsonld/corLdExpandTree.h"                // corLdExpandEntityTree
#include "corNgsild/CorNgsild.h"                      // corNgsild, ldDefaultContext
#include "corNgsild/ldHooks.h"                        // corNgsildFallbackRelease
#include "corNgsild/ldNotifyDefer.h"                  // ldNotifyDispatchPending
#include "corNgsild/ldCsrSubNotify.h"                 // ldCsrSubDispatchPending
#include "corNgsild/ldCheckSubscription.h"            // ldSubEntityTypeExprsRelease

#include "troe/troeDispatch.h"                        // troeDispatchPending
#include "serviceRoutines/postEntityBatchUpsert.h"    // postEntityBatchUpsert
#include "bridge/Channel.h"                           // Channel
#include "bridge/RecordMap.h"                         // RecordMap, RecordEntity, RecordAttr
#include "bridge/recordMap.h"                         // recordTemplateExpand
#include "bridge/bridgeSampleIn.h"                    // bridgeThreadBind
#include "bridge/bridgeServiceSync.h"                 // bridgeRequestsReleasePending
#include "bridge/bridgeRecordIn.h"                    // Own interface
#include "coraineTraceLevels.h"                       // CtBridge



// -----------------------------------------------------------------------------
//
// hasValue - a column with something in it: not absent, not null, not ""
//
static CorNode* hasValue(CorNode* recordP, const char* column)
{
  CorNode* valueP = corTreeLookup(recordP, column);

  if ((valueP == NULL) || (valueP->type == CorNull) || (valueP->type == CorNone))
    return NULL;

  if ((valueP->type == CorString) && (valueP->value.s[0] == 0))
    return NULL;

  return valueP;
}



// -----------------------------------------------------------------------------
//
// numberOf - a number, or a string holding one ("15", " 42.35 ", "-7e2"); false when it is neither
//
static bool numberOf(CorNode* valueP, bool* isIntP, long long* iP, double* fP)
{
  if (valueP->type == CorInt)
  {
    *isIntP = true;
    *iP     = valueP->value.i;
    return true;
  }

  if (valueP->type == CorFloat)
  {
    *isIntP = false;
    *fP     = valueP->value.f;
    return true;
  }

  if (valueP->type != CorString)
    return false;

  const char* s = valueP->value.s;

  while (isspace((unsigned char) *s))
    s++;

  if (*s == 0)
    return false;

  char* endP;

  errno = 0;
  long long i = strtoll(s, &endP, 10);

  while (isspace((unsigned char) *endP))
    endP++;

  if ((*endP == 0) && (errno == 0))
  {
    *isIntP = true;
    *iP     = i;
    return true;
  }

  double f = strtod(s, &endP);

  while (isspace((unsigned char) *endP))
    endP++;

  if ((*endP != 0) || (isfinite(f) == 0))
    return false;

  *isIntP = false;
  *fP     = f;
  return true;
}



// -----------------------------------------------------------------------------
//
// numberNode - numberOf as a node; NULL when the value is no number
//
static CorNode* numberNode(CorNode* valueP, const char* name)
{
  bool      isInt;
  long long i;
  double    f;

  if (numberOf(valueP, &isInt, &i, &f) == false)
    return NULL;

  return (isInt == true) ? corTreeInteger(corRest.kallocP, name, i) : corTreeFloat(corRest.kallocP, name, f);
}



// -----------------------------------------------------------------------------
//
// columnValue - the "value" of a column attribute, as the mapping asks for it; NULL: left out
//
static CorNode* columnValue(RecordAttr* attrP, CorNode* recordP)
{
  CorNode* valueP = hasValue(recordP, attrP->column);

  if (valueP == NULL)
    return NULL;

  switch (attrP->as)
  {
  case RecordAsIs:
  {
    CorNode* copyP = corTreeClone(corRest.kallocP, valueP);

    if (copyP != NULL)
      copyP->name = (char*) "value";

    return copyP;
  }

  case RecordAsNumber:
    return numberNode(valueP, "value");

  case RecordAsBoolean:
    if (valueP->type == CorBoolean)
      return corTreeBoolean(corRest.kallocP, "value", valueP->value.b);

    if (valueP->type == CorInt)
      return ((valueP->value.i == 0) || (valueP->value.i == 1)) ? corTreeBoolean(corRest.kallocP, "value", valueP->value.i == 1) : NULL;

    if (valueP->type == CorString)
    {
      const char* s = valueP->value.s;

      if ((strcasecmp(s, "yes") == 0) || (strcasecmp(s, "true") == 0) || (strcasecmp(s, "y") == 0) || (strcmp(s, "1") == 0))
        return corTreeBoolean(corRest.kallocP, "value", true);

      if ((strcasecmp(s, "no") == 0) || (strcasecmp(s, "false") == 0) || (strcasecmp(s, "n") == 0) || (strcmp(s, "0") == 0))
        return corTreeBoolean(corRest.kallocP, "value", false);
    }
    return NULL;

  case RecordAsString:
  {
    char buf[64];

    switch (valueP->type)
    {
    case CorString:  return corTreeString(corRest.kallocP, "value", valueP->value.s);
    case CorInt:     snprintf(buf, sizeof(buf), "%lld", valueP->value.i);  return corTreeString(corRest.kallocP, "value", corAllocStrdup(&corRest.kalloc, buf));
    case CorFloat:   snprintf(buf, sizeof(buf), "%.15g", valueP->value.f); return corTreeString(corRest.kallocP, "value", corAllocStrdup(&corRest.kalloc, buf));
    case CorBoolean: return corTreeString(corRest.kallocP, "value", (valueP->value.b == true) ? "true" : "false");
    default:         return NULL;
    }
  }
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// pointValue - a GeoJSON Point from two columns; NULL when either is no number, or out of range
//
static CorNode* pointValue(RecordAttr* attrP, CorNode* recordP)
{
  CorNode* lonP = hasValue(recordP, attrP->longitude);
  CorNode* latP = hasValue(recordP, attrP->latitude);

  if ((lonP == NULL) || (latP == NULL))
    return NULL;

  bool      isInt;
  long long i;
  double    lon;
  double    lat;

  if (numberOf(lonP, &isInt, &i, &lon) == false)
    return NULL;
  if (isInt == true)
    lon = (double) i;

  if (numberOf(latP, &isInt, &i, &lat) == false)
    return NULL;
  if (isInt == true)
    lat = (double) i;

  if ((lon < -180) || (lon > 180) || (lat < -90) || (lat > 90))
    return NULL;

  CorNode* valueP       = corTreeObject(corRest.kallocP, "value");
  CorNode* coordinatesP = corTreeArray(corRest.kallocP, "coordinates");

  corTreeChildAdd(coordinatesP, corTreeFloat(corRest.kallocP, NULL, lon));
  corTreeChildAdd(coordinatesP, corTreeFloat(corRest.kallocP, NULL, lat));
  corTreeChildAdd(valueP, corTreeString(corRest.kallocP, "type", "Point"));
  corTreeChildAdd(valueP, coordinatesP);

  return valueP;
}



// -----------------------------------------------------------------------------
//
// attributeMake - one attribute of a record's entity; NULL when the record has nothing for it
//
static CorNode* attributeMake(RecordAttr* attrP, CorNode* recordP, const char* observedAt)
{
  const char* type   = "Property";
  CorNode*    valueP = NULL;

  switch (attrP->kind)
  {
  case RecordAttrColumn:
    valueP = columnValue(attrP, recordP);
    break;

  case RecordAttrPoint:
    type   = "GeoProperty";
    valueP = pointValue(attrP, recordP);
    break;

  case RecordAttrRelationship:
  {
    char* object = recordTemplateExpand(attrP->object, recordP, &corRest.kalloc);

    type   = "Relationship";
    valueP = (object != NULL) ? corTreeString(corRest.kallocP, "object", object) : NULL;
    break;
  }
  }

  if (valueP == NULL)
    return NULL;

  CorNode* attributeP = corTreeObject(corRest.kallocP, attrP->name);

  corTreeChildAdd(attributeP, corTreeString(corRest.kallocP, "type", type));
  corTreeChildAdd(attributeP, valueP);

  if (observedAt != NULL)
    corTreeChildAdd(attributeP, corTreeString(corRest.kallocP, "observedAt", observedAt));

  return attributeP;
}



// -----------------------------------------------------------------------------
//
// entityMake - one entity of the mapping, from a record; NULL when the record cannot fill in its id
//
static CorNode* entityMake(RecordEntity* mappedP, CorNode* recordP, const char* observedAt)
{
  char* id = recordTemplateExpand(mappedP->id, recordP, &corRest.kalloc);

  if (id == NULL)
    return NULL;

  CorNode* entityP = corTreeObject(corRest.kallocP, NULL);

  corTreeChildAdd(entityP, corTreeString(corRest.kallocP, "id",   id));
  corTreeChildAdd(entityP, corTreeString(corRest.kallocP, "type", mappedP->type));

  for (RecordAttr* attrP = mappedP->attrs; attrP != NULL; attrP = attrP->next)
  {
    CorNode* attributeP = attributeMake(attrP, recordP, observedAt);

    if (attributeP != NULL)
      corTreeChildAdd(entityP, attributeP);
  }

  return entityP;
}



// -----------------------------------------------------------------------------
//
// upsert - the batch through the service routine; how many entities were written
//
static int upsert(CorNode* entitiesP, int entities, const char* endpoint)
{
  corRest.in.requestTree         = entitiesP;
  corRest.out.httpStatusCode     = 200;
  corRest.out.headerV            = corRest.out.headers;
  corRest.out.headerCount        = 0;
  corRest.out.headerSize         = sizeof(corRest.out.headers) / sizeof(corRest.out.headers[0]);
  corRest.out.responseTree       = NULL;
  corRest.out.problemType        = NULL;
  corRest.out.problemTitle       = NULL;
  corRest.out.problemDetail[0]   = 0;

  corNgsild.contextP             = ldDefaultContext(&corRest.kalloc);
  corNgsild.userContextBody      = NULL;
  corNgsild.batchPreErrors       = NULL;
  corNgsild.local                = true;              // this broker's store: a record is not forwarded
  corNgsild.upsertUpdate         = true;              // options=update: the attributes the mapping does not name stay

  postEntityBatchUpsert();

  corNgsild.local                = false;
  corNgsild.upsertUpdate         = false;

  //
  // What the request's post-response hook does after a write
  //
  ldNotifyDispatchPending();
  ldCsrSubDispatchPending();
  troeDispatchPending();
  bridgeRequestsReleasePending();
  ldSubEntityTypeExprsRelease();
  corNgsildFallbackRelease();

  int status = corRest.out.httpStatusCode;

  if ((status == 201) || (status == 204))
    return entities;

  if (status != 207)
  {
    COR_W("record on '%s': no entity written (%d: %s)", endpoint, status, corRest.out.problemDetail);
    return 0;
  }

  CorNode* errorsP  = (corRest.out.responseTree != NULL) ? corTreeLookup(corRest.out.responseTree, "errors")  : NULL;
  CorNode* successP = (corRest.out.responseTree != NULL) ? corTreeLookup(corRest.out.responseTree, "success") : NULL;
  int      written  = 0;

  for (CorNode* nodeP = (successP != NULL) ? successP->value.head : NULL; nodeP != NULL; nodeP = nodeP->next)
    written++;

  for (CorNode* errP = (errorsP != NULL) ? errorsP->value.head : NULL; errP != NULL; errP = errP->next)
  {
    CorNode* idP     = corTreeLookup(errP, "entityId");
    CorNode* pdP     = corTreeLookup(errP, "error");
    CorNode* detailP = (pdP != NULL) ? corTreeLookup(pdP, "detail") : NULL;

    COR_W("record on '%s': entity '%s' not written: %s", endpoint,
          ((idP     != NULL) && (idP->type     == CorString)) ? idP->value.s     : "?",
          ((detailP != NULL) && (detailP->type == CorString)) ? detailP->value.s : "?");
  }

  return written;
}



// -----------------------------------------------------------------------------
//
// bridgeRecordIn -
//
int bridgeRecordIn(Channel* channelP, const char* json, int64_t publishTime)
{
  bridgeThreadBind(channelP->tenantP);

  char*    text    = corAllocStrdup(&corRest.kalloc, json);
  CorNode* recordP = (text != NULL) ? corJsonParse(corRest.corJsonP, text) : NULL;

  if ((recordP == NULL) || (recordP->type != CorObject))
  {
    COR_W("bridge '%s': the record on '%s' is not a JSON object - dropped", channelP->bridgeName, channelP->endpoint);
    corNgsildFallbackRelease();
    return BRIDGE_BAD_INPUT;
  }

  char        observedAtBuf[32];
  const char* observedAt = NULL;

  if (publishTime > 0)
  {
    corTimeIso(publishTime, 3, true, observedAtBuf);
    observedAt = observedAtBuf;
  }

  CorNode* entitiesP = corTreeArray(corRest.kallocP, NULL);
  int      entities  = 0;

  for (RecordEntity* mappedP = channelP->recordMapP->entities; mappedP != NULL; mappedP = mappedP->next)
  {
    CorNode* entityP = entityMake(mappedP, recordP, observedAt);

    if (entityP != NULL)
    {
      corTreeChildAdd(entitiesP, entityP);
      entities++;
    }
  }

  if (entities == 0)
  {
    COR_T(CtBridge, "record on '%s' makes no entity - a column an id needs has no value", channelP->endpoint);
    corNgsildFallbackRelease();
    return BRIDGE_BAD_INPUT;
  }

  corLdExpandEntityTree(entitiesP, ldDefaultContext(&corRest.kalloc), &corRest.kalloc);

  int written = upsert(entitiesP, entities, channelP->endpoint);

  if (written == 0)
    return BRIDGE_ERR;

  __atomic_add_fetch(&channelP->samplesIn, 1, __ATOMIC_RELAXED);
  COR_T(CtBridge, "record on '%s' -> %d entit%s", channelP->endpoint, written, (written == 1) ? "y" : "ies");

  return BRIDGE_OK;
}
