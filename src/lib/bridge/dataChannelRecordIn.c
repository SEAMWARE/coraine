//
// FILE            dataChannelRecordIn.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A DataChannel's record -> the entities its mapping makes -> one batch upsert. See dataChannelRecordIn.h.
//
// The write is bridgeEntitiesUpsert's - the service routine of POST /entityOperations/upsert, in this
// process: an entity from a record is checked, stored, notified and recorded in the temporal store exactly
// as one a client sends.
//
#include <ctype.h>                                    // isspace
#include <errno.h>                                    // errno
#include <math.h>                                     // isfinite
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf
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
#include "corNgsild/ldCheckDateTime.h"                // ldCheckDateTime, ldIsoToNanoseconds
#include "corNgsild/ldNameContentCheck.h"             // ldIsValidName
#include "corJsonld/corLdExpand.h"                    // corLdExpand

#include "bridge/DataChannel.h"                       // DataChannel
#include "bridge/RecordMap.h"                         // RecordMap, RecordEntity, RecordAttr
#include "bridge/recordMap.h"                         // recordTemplateExpand
#include "bridge/bridgeSampleIn.h"                    // bridgeThreadBind
#include "bridge/bridgeEntitiesUpsert.h"              // bridgeEntitiesUpsert
#include "bridge/dataChannelRecordIn.h"               // Own interface
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
// jsonOf - a string holding JSON, parsed; NULL when it does not parse. The node is named 'name'
//
static CorNode* jsonOf(const char* text, const char* name)
{
  char* copy = corAllocStrdup(&corRest.kalloc, text);    // the parser writes into its buffer
  CorNode* nodeP = (copy != NULL) ? corJsonParse(corRest.corJsonP, copy) : NULL;

  if (nodeP != NULL)
    nodeP->name = (char*) name;

  return nodeP;
}



// -----------------------------------------------------------------------------
//
// cloneAs - a copy of a value, named 'name'
//
static CorNode* cloneAs(CorNode* valueP, const char* name)
{
  CorNode* copyP = corTreeClone(corRest.kallocP, valueP);

  if (copyP != NULL)
    copyP->name = (char*) name;

  return copyP;
}



// -----------------------------------------------------------------------------
//
// dateTimeValue - an ISO 8601 date-time, normalised to UTC (milliseconds, when it has them), as a DateTime value:
// { "@type": "DateTime", "@value": "2026-10-20T21:00:00.000Z" }; NULL when it is not one
//
static CorNode* dateTimeValue(CorNode* valueP)
{
  if ((valueP->type != CorString) || (ldCheckDateTime(valueP->value.s, NULL) == false))
    return NULL;

  char buf[40];

  corTimeIso(ldIsoToNanoseconds(valueP->value.s), 3, false, buf);

  CorNode* dtP = corTreeObject(corRest.kallocP, "value");

  corTreeChildAdd(dtP, corTreeString(corRest.kallocP, "@type", "DateTime"));
  corTreeChildAdd(dtP, corTreeString(corRest.kallocP, "@value", corAllocStrdup(&corRest.kalloc, buf)));

  return dtP;
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
    return cloneAs(valueP, "value");

  case RecordAsNumber:
    return numberNode(valueP, "value");

  case RecordAsInteger:
  {
    bool      isInt;
    long long i;
    double    f;

    if (numberOf(valueP, &isInt, &i, &f) == false)
      return NULL;

    if (isInt == true)
      return corTreeInteger(corRest.kallocP, "value", i);

    if ((f != (double) (long long) f) || (f > 9.2e18) || (f < -9.2e18))   // not whole, or not a long long
      return NULL;

    return corTreeInteger(corRest.kallocP, "value", (long long) f);
  }

  case RecordAsFloat:
  {
    bool      isInt;
    long long i;
    double    f;

    if (numberOf(valueP, &isInt, &i, &f) == false)
      return NULL;

    return corTreeFloat(corRest.kallocP, "value", (isInt == true) ? (double) i : f);
  }

  case RecordAsDateTime:
    return dateTimeValue(valueP);

  case RecordAsJson:
    return (valueP->type == CorString) ? jsonOf(valueP->value.s, "value") : cloneAs(valueP, "value");

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
// geoValue - a GeoJSON geometry from one column: "lat, lng" (a Point, as an IoT Agent's geo:point), a
// GeoJSON object, or its text; NULL when it is none of them. That it is a valid geometry is checked where
// every GeoProperty is, on the write
//
static CorNode* geoValue(RecordAttr* attrP, CorNode* recordP)
{
  CorNode* valueP = hasValue(recordP, attrP->column);

  if (valueP == NULL)
    return NULL;

  if (valueP->type == CorObject)
    return cloneAs(valueP, "value");

  if (valueP->type != CorString)
    return NULL;

  const char* s = valueP->value.s;

  while (isspace((unsigned char) *s))
    s++;

  if (*s == '{')
  {
    CorNode* geoP = jsonOf(s, "value");

    return ((geoP != NULL) && (geoP->type == CorObject)) ? geoP : NULL;
  }

  //
  // "lat, lng" - latitude FIRST, as an IoT Agent's geo:point says it; GeoJSON's order is the other way round
  //
  char*  endP;
  double lat = strtod(s, &endP);

  if (endP == s)
    return NULL;

  while (isspace((unsigned char) *endP))
    endP++;

  if (*endP != ',')
    return NULL;

  const char* lonText = endP + 1;
  double      lon     = strtod(lonText, &endP);

  if (endP == lonText)
    return NULL;

  while (isspace((unsigned char) *endP))
    endP++;

  if ((*endP != 0) || (isfinite(lat) == 0) || (isfinite(lon) == 0) || (lon < -180) || (lon > 180) || (lat < -90) || (lat > 90))
    return NULL;

  CorNode* pointP       = corTreeObject(corRest.kallocP, "value");
  CorNode* coordinatesP = corTreeArray(corRest.kallocP, "coordinates");

  corTreeChildAdd(coordinatesP, corTreeFloat(corRest.kallocP, NULL, lon));
  corTreeChildAdd(coordinatesP, corTreeFloat(corRest.kallocP, NULL, lat));
  corTreeChildAdd(pointP, corTreeString(corRest.kallocP, "type", "Point"));
  corTreeChildAdd(pointP, coordinatesP);

  return pointP;
}



// -----------------------------------------------------------------------------
//
// attributeMake - one attribute of a record's entity; NULL when the record has nothing for it
//
static CorNode* attributeMake(RecordAttr* attrP, CorNode* recordP, RecordIdEncoding idEncoding, const char* observedAt)
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
    char* object = recordTemplateExpand(attrP->object, recordP, idEncoding, &corRest.kalloc);

    type   = "Relationship";
    valueP = (object != NULL) ? corTreeString(corRest.kallocP, "object", object) : NULL;
    break;
  }

  case RecordAttrGeo:
    type   = "GeoProperty";
    valueP = geoValue(attrP, recordP);
    break;

  case RecordAttrJson:
  {
    CorNode* columnP = hasValue(recordP, attrP->column);

    type   = "JsonProperty";
    valueP = (columnP != NULL) ? cloneAs(columnP, "json") : NULL;
    break;
  }

  case RecordAttrVocab:
  {
    CorNode* columnP = hasValue(recordP, attrP->column);

    type   = "VocabProperty";
    valueP = ((columnP != NULL) && ((columnP->type == CorString) || (columnP->type == CorArray))) ? cloneAs(columnP, "vocab") : NULL;
    break;
  }

  case RecordAttrLanguageMap:
  {
    CorNode* columnP = hasValue(recordP, attrP->column);

    type = "LanguageProperty";

    if ((columnP != NULL) && (columnP->type == CorString))
      columnP = jsonOf(columnP->value.s, "languageMap");                     // the map as text

    valueP = ((columnP != NULL) && (columnP->type == CorObject)) ? cloneAs(columnP, "languageMap") : NULL;
    break;
  }

  case RecordAttrConstant:
    valueP     = jsonOf(attrP->constant, "value");
    observedAt = NULL;                                                     // the mapping's, not a reading
    break;
  }

  if (valueP == NULL)
    return NULL;

  CorNode* attributeP = corTreeObject(corRest.kallocP, attrP->name);

  corTreeChildAdd(attributeP, corTreeString(corRest.kallocP, "type", type));
  corTreeChildAdd(attributeP, valueP);

  if (attrP->unitCode != NULL)
    corTreeChildAdd(attributeP, corTreeString(corRest.kallocP, "unitCode", attrP->unitCode));

  if (observedAt != NULL)
    corTreeChildAdd(attributeP, corTreeString(corRest.kallocP, "observedAt", observedAt));

  return attributeP;
}



// -----------------------------------------------------------------------------
//
// entityMake - one entity of the mapping, from a record; NULL when the record cannot fill in its id
//
static CorNode* entityMake(RecordEntity* mappedP, CorNode* recordP, RecordIdEncoding idEncoding, const char* observedAt)
{
  char* id = recordTemplateExpand(mappedP->id, recordP, idEncoding, &corRest.kalloc);

  if (id == NULL)
    return NULL;

  CorNode* entityP = corTreeObject(corRest.kallocP, NULL);

  corTreeChildAdd(entityP, corTreeString(corRest.kallocP, "id",   id));
  corTreeChildAdd(entityP, corTreeString(corRest.kallocP, "type", mappedP->type));

  if (mappedP->scope != NULL)
  {
    //
    // A scope keeps its '/' - only what a column puts in is encoded. A column without a value: no scope,
    // the entity is still made
    //
    char* scope = recordTemplateExpand(mappedP->scope, recordP, idEncoding, &corRest.kalloc);

    if (scope != NULL)
      corTreeChildAdd(entityP, corTreeString(corRest.kallocP, "scope", scope));
  }

  for (RecordAttr* attrP = mappedP->attrs; attrP != NULL; attrP = attrP->next)
  {
    CorNode* attributeP = attributeMake(attrP, recordP, idEncoding, observedAt);

    if (attributeP != NULL)
      corTreeChildAdd(entityP, attributeP);
  }

  return entityP;
}



// -----------------------------------------------------------------------------
//
// otherColumnsAdd - otherColumns "property": every column the mapping does not use becomes a Property of
// its own name on the record's first entity. Not one: a column whose name starts with '@' (the bridge's - an
// IoT Agent's @deviceId), 'id' and 'type' (the entity's own), a name NGSI-LD does not allow (§ 4.6.2), a
// column with no value, and one whose name is an attribute the mapping already made
//
static void otherColumnsAdd(DataChannel* dcP, CorNode* recordP, CorNode* entityP, const char* observedAt)
{
  for (CorNode* columnP = recordP->value.head; columnP != NULL; columnP = columnP->next)
  {
    const char* name = columnP->name;

    if ((name == NULL) || (name[0] == '@') || (strcmp(name, "id") == 0) || (strcmp(name, "type") == 0))
      continue;

    if ((recordColumnUsed(dcP->mapP, name) == true) || (hasValue(recordP, name) == NULL))
      continue;

    if (ldIsValidName(name) == false)
    {
      COR_T(CtBridge, "record on DataChannel '%s': column '%s' is no attribute name - left out", dcP->endpoint, name);
      continue;
    }

    if ((columnP->type == CorFloat) && (isfinite(columnP->value.f) == 0))
      continue;

    //
    // The mapped attributes' names are expanded already; the column's is, once the entity is
    //
    char* expanded = corLdExpand(ldDefaultContext(&corRest.kalloc), name, &corRest.kalloc, NULL, NULL);
    bool  taken    = false;

    for (CorNode* memberP = entityP->value.head; memberP != NULL; memberP = memberP->next)
    {
      if ((strcmp(memberP->name, name) == 0) || ((expanded != NULL) && (strcmp(memberP->name, expanded) == 0)))
      {
        taken = true;
        break;
      }
    }

    if (taken == true)
      continue;

    CorNode* attributeP = corTreeObject(corRest.kallocP, name);

    corTreeChildAdd(attributeP, corTreeString(corRest.kallocP, "type", "Property"));
    corTreeChildAdd(attributeP, cloneAs(columnP, "value"));

    if (observedAt != NULL)
      corTreeChildAdd(attributeP, corTreeString(corRest.kallocP, "observedAt", observedAt));

    corTreeChildAdd(entityP, attributeP);
  }
}



// -----------------------------------------------------------------------------
//
// dataChannelRecordIn -
//
int dataChannelRecordIn(DataChannel* dcP, const char* json, int64_t publishTime)
{
  bridgeThreadBind(dcP->tenantP);

  char*    text    = corAllocStrdup(&corRest.kalloc, json);
  CorNode* recordP = (text != NULL) ? corJsonParse(corRest.corJsonP, text) : NULL;

  if ((recordP == NULL) || (recordP->type != CorObject))
  {
    COR_W("bridge '%s': the record on DataChannel '%s' is not a JSON object - dropped", dcP->bridgeName, dcP->endpoint);
    __atomic_add_fetch(&dcP->recordsRefused, 1, __ATOMIC_RELAXED);
    corNgsildFallbackRelease();
    return BRIDGE_BAD_INPUT;
  }

  //
  // The record's time, if its bridge said one (a CKAN row's timeColumn) - else no observedAt at all
  //
  char        observedAtBuf[32];
  const char* observedAt = NULL;

  if (publishTime > 0)
  {
    corTimeIso(publishTime, 3, true, observedAtBuf);
    observedAt = observedAtBuf;
  }

  //
  // Each entity of the mapping the record can name. One that cannot - a column its id needs has no value -
  // is not made, and does not take the others with it
  //
  CorNode* entitiesP = corTreeArray(corRest.kallocP, NULL);
  int      entities  = 0;

  for (RecordEntity* mappedP = dcP->mapP->entities; mappedP != NULL; mappedP = mappedP->next)
  {
    CorNode* entityP = entityMake(mappedP, recordP, dcP->mapP->idEncoding, observedAt);

    if (entityP != NULL)
    {
      corTreeChildAdd(entitiesP, entityP);
      entities++;
    }
  }

  if ((entities > 0) && (dcP->mapP->otherColumns == RecordOtherProperty))
    otherColumnsAdd(dcP, recordP, entitiesP->value.head, observedAt);

  if (entities == 0)
  {
    COR_T(CtBridge, "record on DataChannel '%s' makes no entity - a column an id needs has no value", dcP->endpoint);
    __atomic_add_fetch(&dcP->recordsRefused, 1, __ATOMIC_RELAXED);
    corNgsildFallbackRelease();
    return BRIDGE_BAD_INPUT;
  }

  corLdExpandEntityTree(entitiesP, ldDefaultContext(&corRest.kalloc), &corRest.kalloc);

  char what[256];

  snprintf(what, sizeof(what), "record on DataChannel '%s'", dcP->endpoint);

  int written = bridgeEntitiesUpsert(entitiesP, entities, what);

  if (written == 0)
  {
    __atomic_add_fetch(&dcP->recordsRefused, 1, __ATOMIC_RELAXED);
    return BRIDGE_ERR;
  }

  __atomic_add_fetch(&dcP->recordsIn, 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&dcP->entitiesWritten, (uint64_t) written, __ATOMIC_RELAXED);
  COR_T(CtBridge, "record on DataChannel '%s' -> %d entit%s", dcP->endpoint, written, (written == 1) ? "y" : "ies");

  return BRIDGE_OK;
}
