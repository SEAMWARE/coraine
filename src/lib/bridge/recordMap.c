//
// FILE            recordMap.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A DataChannel's mapping: read from the configuration file, and its id / object templates expanded
// over a record. See RecordMap.h for what the mapping says.
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // malloc, calloc, realloc, free
#include <string.h>                                   // strcmp, strncmp, strdup, strchr, strlen, memcpy

#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corJson/corJsonRender.h"                    // corJsonFastRender
#include "corJson/corJsonRenderSize.h"                // corJsonFastRenderSize

#include "corJsonld/corLdExpand.h"                    // corLdExpand
#include "corNgsild/CorNgsild.h"                      // ldDefaultContext

#include "bridge/RecordMap.h"                         // RecordMap, RecordEntity, RecordAttr
#include "bridge/recordMap.h"                         // Own interface



// -----------------------------------------------------------------------------
//
// stringMember - a non-empty string member, or NULL
//
static const char* stringMember(CorNode* objectP, const char* name)
{
  CorNode* nodeP = corTreeLookup(objectP, name);

  if ((nodeP == NULL) || (nodeP->type != CorString) || (nodeP->value.s == NULL) || (nodeP->value.s[0] == 0))
    return NULL;

  return nodeP->value.s;
}



// -----------------------------------------------------------------------------
//
// templateCheck - "{Column}" pairs, each naming a column; NULL when fine, else why not
//
static const char* templateCheck(const char* templ)
{
  for (const char* cP = templ; *cP != 0; cP++)
  {
    if (*cP == '}')
      return "a '}' with no '{' before it";

    if (*cP != '{')
      continue;

    const char* endP = strchr(cP + 1, '}');

    if (endP == NULL)
      return "a '{' with no '}' after it";

    if (endP == cP + 1)
      return "'{}' names no column";

    for (const char* nP = cP + 1; nP < endP; nP++)
    {
      if (*nP == '{')
        return "a '{' inside '{...}'";
    }

    cP = endP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// columnAdd - a column the mapping uses, once; false: out of memory
//
static bool columnAdd(RecordMap* mapP, const char* column, int len)
{
  for (int ix = 0; ix < mapP->columns; ix++)
  {
    if ((strncmp(mapP->columnV[ix], column, len) == 0) && (mapP->columnV[ix][len] == 0))
      return true;
  }

  char** columnV = (char**) realloc(mapP->columnV, (mapP->columns + 1) * sizeof(char*));

  if (columnV == NULL)
    return false;

  mapP->columnV = columnV;

  char* name = (char*) malloc(len + 1);

  if (name == NULL)
    return false;

  memcpy(name, column, len);
  name[len] = 0;

  mapP->columnV[mapP->columns++] = name;
  return true;
}



// -----------------------------------------------------------------------------
//
// templateColumnsAdd - the columns a (checked) template names
//
static bool templateColumnsAdd(RecordMap* mapP, const char* templ)
{
  for (const char* cP = strchr(templ, '{'); cP != NULL; cP = strchr(cP + 1, '{'))
  {
    const char* endP = strchr(cP + 1, '}');

    if ((endP == NULL) || (columnAdd(mapP, cP + 1, endP - cP - 1) == false))
      return false;

    cP = endP;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// nodeText - a node as JSON text, rendered detached (corJsonFastRender follows the sibling chain and
// renders a name); malloc'd
//
static char* nodeText(CorNode* nodeP)
{
  char*    name = nodeP->name;
  CorNode* next = nodeP->next;

  nodeP->name = NULL;
  nodeP->next = NULL;

  int   size = corJsonFastRenderSize(nodeP);
  char* text = (char*) malloc(size + 1);

  if (text != NULL)
    corJsonFastRender(nodeP, text);

  nodeP->name = name;
  nodeP->next = next;

  return text;
}



// -----------------------------------------------------------------------------
//
// expandedName - an attribute name or an entity type, expanded the way a plain Channel's are (the
// deployment's default user context, else core); malloc'd
//
static char* expandedName(const char* name, CorAlloc* kaP)
{
  char* expanded = corLdExpand(ldDefaultContext(kaP), name, kaP, NULL, NULL);

  return (expanded != NULL) ? strdup(expanded) : NULL;
}



// -----------------------------------------------------------------------------
//
// attrFree / entityFree -
//
static void attrFree(RecordAttr* attrP)
{
  free(attrP->name);
  free(attrP->column);
  free(attrP->longitude);
  free(attrP->latitude);
  free(attrP->object);
  free(attrP->constant);
  free(attrP->unitCode);
  free(attrP);
}

static void entityFree(RecordEntity* entityP)
{
  RecordAttr* attrP = entityP->attrs;

  while (attrP != NULL)
  {
    RecordAttr* nextP = attrP->next;
    attrFree(attrP);
    attrP = nextP;
  }

  free(entityP->type);
  free(entityP->id);
  free(entityP->scope);
  free(entityP);
}



// -----------------------------------------------------------------------------
//
// attrParse - one member of an entity's "attributes"
//
//   { "column": "Name" }   { "column": "Total_docks", "as": "number" }
//   { "point": { "longitude": "Longitude", "latitude": "Latitude" } }
//   { "relationship": "urn:ngsi-ld:District:{District}" }
//   { "geo": "loc" }  { "json": "cfg" }  { "vocab": "state" }  { "languageMap": "label" }  { "value": 21 }
//   "unitCode" with "column" and "value"
//
// Every column it uses is added to the map's (columnAdd) - those are not "other" columns.
//
static RecordAttr* attrParse(CorNode* attrNodeP, RecordMap* mapP, CorAlloc* kaP, const char** whyP)
{
  static char why[256];

  if (attrNodeP->type != CorObject)
  {
    snprintf(why, sizeof(why), "attribute '%s' is not an object", attrNodeP->name);
    *whyP = why;
    return NULL;
  }

  if ((strcmp(attrNodeP->name, "id") == 0) || (strcmp(attrNodeP->name, "type") == 0))
  {
    snprintf(why, sizeof(why), "'%s' is the entity's own, not an attribute", attrNodeP->name);
    *whyP = why;
    return NULL;
  }

  CorNode*    columnP       = corTreeLookup(attrNodeP, "column");
  CorNode*    pointP        = corTreeLookup(attrNodeP, "point");
  CorNode*    relationshipP = corTreeLookup(attrNodeP, "relationship");
  CorNode*    geoP          = corTreeLookup(attrNodeP, "geo");
  CorNode*    jsonP         = corTreeLookup(attrNodeP, "json");
  CorNode*    vocabP        = corTreeLookup(attrNodeP, "vocab");
  CorNode*    languageMapP  = corTreeLookup(attrNodeP, "languageMap");
  CorNode*    constantP     = corTreeLookup(attrNodeP, "value");
  int         ways          = (columnP != NULL) + (pointP != NULL) + (relationshipP != NULL) + (geoP != NULL) +
                              (jsonP != NULL) + (vocabP != NULL) + (languageMapP != NULL) + (constantP != NULL);

  if (ways != 1)
  {
    snprintf(why, sizeof(why), "attribute '%s' must say exactly one of 'column', 'point', 'relationship', 'geo', 'json', 'vocab', 'languageMap' and 'value'", attrNodeP->name);
    *whyP = why;
    return NULL;
  }

  for (CorNode* memberP = attrNodeP->value.head; memberP != NULL; memberP = memberP->next)
  {
    if ((strcmp(memberP->name, "column") != 0) && (strcmp(memberP->name, "as")           != 0) &&
        (strcmp(memberP->name, "point")  != 0) && (strcmp(memberP->name, "relationship") != 0) &&
        (strcmp(memberP->name, "geo")    != 0) && (strcmp(memberP->name, "json")         != 0) &&
        (strcmp(memberP->name, "vocab")  != 0) && (strcmp(memberP->name, "languageMap")  != 0) &&
        (strcmp(memberP->name, "value")  != 0) && (strcmp(memberP->name, "unitCode")     != 0))
    {
      snprintf(why, sizeof(why), "attribute '%s': unknown member '%s'", attrNodeP->name, memberP->name);
      *whyP = why;
      return NULL;
    }
  }

  RecordAttr* attrP = (RecordAttr*) calloc(1, sizeof(RecordAttr));

  if (attrP == NULL)
  {
    *whyP = "out of memory";
    return NULL;
  }

  attrP->name = expandedName(attrNodeP->name, kaP);

  if (attrP->name == NULL)
  {
    snprintf(why, sizeof(why), "attribute '%s' cannot be expanded", attrNodeP->name);
    *whyP = why;
    attrFree(attrP);
    return NULL;
  }

  const char* as = stringMember(attrNodeP, "as");

  if ((as != NULL) && (columnP == NULL))
  {
    snprintf(why, sizeof(why), "attribute '%s': 'as' goes with 'column'", attrNodeP->name);
    *whyP = why;
    attrFree(attrP);
    return NULL;
  }

  CorNode* unitCodeP = corTreeLookup(attrNodeP, "unitCode");

  if (unitCodeP != NULL)
  {
    if ((columnP == NULL) && (constantP == NULL))
    {
      snprintf(why, sizeof(why), "attribute '%s': 'unitCode' goes with 'column' and 'value' - a Property", attrNodeP->name);
      *whyP = why;
      attrFree(attrP);
      return NULL;
    }

    if ((unitCodeP->type != CorString) || (unitCodeP->value.s[0] == 0))
    {
      snprintf(why, sizeof(why), "attribute '%s': 'unitCode' is a non-empty string (a UN/CEFACT code: \"CEL\")", attrNodeP->name);
      *whyP = why;
      attrFree(attrP);
      return NULL;
    }

    attrP->unitCode = strdup(unitCodeP->value.s);
  }

  //
  // geo, json, vocab, languageMap - one column each, as 'column'
  //
  CorNode* oneColumnP = (geoP != NULL) ? geoP : (jsonP != NULL) ? jsonP : (vocabP != NULL) ? vocabP : languageMapP;

  if (oneColumnP != NULL)
  {
    attrP->kind = (geoP != NULL) ? RecordAttrGeo : (jsonP != NULL) ? RecordAttrJson : (vocabP != NULL) ? RecordAttrVocab : RecordAttrLanguageMap;

    if ((oneColumnP->type != CorString) || (oneColumnP->value.s[0] == 0))
    {
      snprintf(why, sizeof(why), "attribute '%s': '%s' is the name of a column - a non-empty string", attrNodeP->name, oneColumnP->name);
      *whyP = why;
      attrFree(attrP);
      return NULL;
    }

    attrP->column = strdup(oneColumnP->value.s);

    if (columnAdd(mapP, attrP->column, strlen(attrP->column)) == false)
      *whyP = "out of memory";
  }
  else if (constantP != NULL)
  {
    attrP->kind = RecordAttrConstant;

    if ((constantP->type == CorNull) || (constantP->type == CorNone))
    {
      snprintf(why, sizeof(why), "attribute '%s': 'value' is the attribute's value - not null", attrNodeP->name);
      *whyP = why;
    }
    else if ((attrP->constant = nodeText(constantP)) == NULL)
      *whyP = "out of memory";
  }
  else

  if (columnP != NULL)
  {
    attrP->kind = RecordAttrColumn;

    if ((columnP->type != CorString) || (columnP->value.s[0] == 0))
      *whyP = "a 'column' is the name of a column - a non-empty string";
    else
    {
      attrP->column = strdup(columnP->value.s);

      if (columnAdd(mapP, attrP->column, strlen(attrP->column)) == false)
        *whyP = "out of memory";
    }

    if (as == NULL)                           attrP->as = RecordAsIs;
    else if (strcmp(as, "string")   == 0)     attrP->as = RecordAsString;
    else if (strcmp(as, "number")   == 0)     attrP->as = RecordAsNumber;
    else if (strcmp(as, "boolean")  == 0)     attrP->as = RecordAsBoolean;
    else if (strcmp(as, "integer")  == 0)     attrP->as = RecordAsInteger;
    else if (strcmp(as, "float")    == 0)     attrP->as = RecordAsFloat;
    else if (strcmp(as, "datetime") == 0)     attrP->as = RecordAsDateTime;
    else if (strcmp(as, "json")     == 0)     attrP->as = RecordAsJson;
    else
    {
      snprintf(why, sizeof(why), "attribute '%s': 'as' is one of string, number, boolean, integer, float, datetime and json", attrNodeP->name);
      *whyP = why;
    }
  }
  else if (pointP != NULL)
  {
    attrP->kind = RecordAttrPoint;

    const char* longitude = (pointP->type == CorObject) ? stringMember(pointP, "longitude") : NULL;
    const char* latitude  = (pointP->type == CorObject) ? stringMember(pointP, "latitude")  : NULL;

    if ((longitude == NULL) || (latitude == NULL))
    {
      snprintf(why, sizeof(why), "attribute '%s': 'point' is { \"longitude\": <column>, \"latitude\": <column> }", attrNodeP->name);
      *whyP = why;
    }
    else
    {
      attrP->longitude = strdup(longitude);
      attrP->latitude  = strdup(latitude);

      if ((columnAdd(mapP, longitude, strlen(longitude)) == false) || (columnAdd(mapP, latitude, strlen(latitude)) == false))
        *whyP = "out of memory";
    }
  }
  else
  {
    attrP->kind = RecordAttrRelationship;

    const char* bad = (relationshipP->type == CorString) ? templateCheck(relationshipP->value.s) : "it is not a string";

    if ((bad == NULL) && (relationshipP->value.s[0] == 0))
      bad = "it is empty";

    if (bad != NULL)
    {
      snprintf(why, sizeof(why), "attribute '%s': 'relationship' is a template of the object's id - %s", attrNodeP->name, bad);
      *whyP = why;
    }
    else
    {
      attrP->object = strdup(relationshipP->value.s);

      if (templateColumnsAdd(mapP, attrP->object) == false)
        *whyP = "out of memory";
    }
  }

  if (*whyP != NULL)
  {
    attrFree(attrP);
    return NULL;
  }

  return attrP;
}



// -----------------------------------------------------------------------------
//
// entityParse - one item of "entities"
//
static RecordEntity* entityParse(CorNode* itemP, RecordMap* mapP, CorAlloc* kaP, const char** whyP)
{
  static char why[256];

  if (itemP->type != CorObject)
  {
    *whyP = "an item of 'entities' is not an object";
    return NULL;
  }

  const char* type = stringMember(itemP, "type");
  const char* id   = stringMember(itemP, "id");

  if ((type == NULL) || (id == NULL))
  {
    *whyP = "an item of 'entities' needs a 'type' and an 'id' (a template: \"urn:ngsi-ld:Station:{Number}\")";
    return NULL;
  }

  const char* bad = templateCheck(id);

  if (bad != NULL)
  {
    snprintf(why, sizeof(why), "the id template '%s': %s", id, bad);
    *whyP = why;
    return NULL;
  }

  for (CorNode* memberP = itemP->value.head; memberP != NULL; memberP = memberP->next)
  {
    if ((strcmp(memberP->name, "type")       != 0) && (strcmp(memberP->name, "id")    != 0) &&
        (strcmp(memberP->name, "attributes") != 0) && (strcmp(memberP->name, "scope") != 0))
    {
      snprintf(why, sizeof(why), "entity '%s': unknown member '%s'", id, memberP->name);
      *whyP = why;
      return NULL;
    }
  }

  RecordEntity* entityP = (RecordEntity*) calloc(1, sizeof(RecordEntity));

  if (entityP == NULL)
  {
    *whyP = "out of memory";
    return NULL;
  }

  entityP->id   = strdup(id);
  entityP->type = expandedName(type, kaP);

  if (templateColumnsAdd(mapP, id) == false)
  {
    *whyP = "out of memory";
    entityFree(entityP);
    return NULL;
  }

  //
  // The scope - a template too: "/garden/{Zone}". What a scope may hold is checked where every entity's is,
  // on the write
  //
  CorNode* scopeP = corTreeLookup(itemP, "scope");

  if (scopeP != NULL)
  {
    const char* badScope = ((scopeP->type == CorString) && (scopeP->value.s[0] != 0)) ? templateCheck(scopeP->value.s) : "it is not a non-empty string";

    if (badScope != NULL)
    {
      snprintf(why, sizeof(why), "entity '%s': 'scope' is a template of the entity's scope (\"/garden/{Zone}\") - %s", id, badScope);
      *whyP = why;
      entityFree(entityP);
      return NULL;
    }

    entityP->scope = strdup(scopeP->value.s);

    if (templateColumnsAdd(mapP, entityP->scope) == false)
    {
      *whyP = "out of memory";
      entityFree(entityP);
      return NULL;
    }
  }

  if (entityP->type == NULL)
  {
    snprintf(why, sizeof(why), "the entity type '%s' cannot be expanded", type);
    *whyP = why;
    entityFree(entityP);
    return NULL;
  }

  CorNode* attrsP = corTreeLookup(itemP, "attributes");

  if ((attrsP != NULL) && (attrsP->type != CorObject))
  {
    snprintf(why, sizeof(why), "entity '%s': 'attributes' is an object - one member per attribute", id);
    *whyP = why;
    entityFree(entityP);
    return NULL;
  }

  RecordAttr* lastP = NULL;

  for (CorNode* attrNodeP = (attrsP != NULL) ? attrsP->value.head : NULL; attrNodeP != NULL; attrNodeP = attrNodeP->next)
  {
    RecordAttr* attrP = attrParse(attrNodeP, mapP, kaP, whyP);

    if (attrP == NULL)
    {
      entityFree(entityP);
      return NULL;
    }

    if (lastP == NULL)
      entityP->attrs = attrP;
    else
      lastP->next = attrP;

    lastP = attrP;
  }

  return entityP;
}



// -----------------------------------------------------------------------------
//
// recordOtherParse -
//
RecordOther recordOtherParse(CorNode* nodeP, const char** whyP)
{
  *whyP = NULL;

  if (nodeP == NULL)
    return RecordOtherUnset;

  if ((nodeP->type == CorString) && (strcmp(nodeP->value.s, "property") == 0))
    return RecordOtherProperty;

  if ((nodeP->type == CorString) && (strcmp(nodeP->value.s, "none") == 0))
    return RecordOtherNone;

  *whyP = "'otherColumns' is \"none\" (the default) or \"property\"";
  return RecordOtherUnset;
}



// -----------------------------------------------------------------------------
//
// recordColumnUsed -
//
bool recordColumnUsed(RecordMap* mapP, const char* column)
{
  for (int ix = 0; ix < mapP->columns; ix++)
  {
    if (strcmp(mapP->columnV[ix], column) == 0)
      return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// recordMapParse -
//
RecordMap* recordMapParse(CorNode* entryP, CorAlloc* kaP, const char** whyP)
{
  *whyP = NULL;

  if (entryP->type != CorObject)
  {
    *whyP = "the entry is not an object";
    return NULL;
  }

  for (CorNode* memberP = entryP->value.head; memberP != NULL; memberP = memberP->next)
  {
    if ((strcmp(memberP->name, "entities")     != 0) && (strcmp(memberP->name, "channelInfo") != 0) &&
        (strcmp(memberP->name, "idEncoding")   != 0) && (strcmp(memberP->name, "otherColumns") != 0))
    {
      static char why[256];

      snprintf(why, sizeof(why), "unknown member '%s' (a DataChannel has 'entities', 'idEncoding', 'otherColumns' and 'channelInfo')", memberP->name);
      *whyP = why;
      return NULL;
    }
  }

  CorNode*         encodingP  = corTreeLookup(entryP, "idEncoding");
  RecordIdEncoding idEncoding = RecordIdMerge;

  if (encodingP != NULL)
  {
    if ((encodingP->type == CorString) && (strcmp(encodingP->value.s, "merge") == 0))
      idEncoding = RecordIdMerge;
    else if ((encodingP->type == CorString) && (strcmp(encodingP->value.s, "percent") == 0))
      idEncoding = RecordIdPercent;
    else
    {
      *whyP = "'idEncoding' is \"merge\" (the default) or \"percent\"";
      return NULL;
    }
  }

  RecordOther otherColumns = recordOtherParse(corTreeLookup(entryP, "otherColumns"), whyP);

  if (*whyP != NULL)
    return NULL;

  CorNode* entitiesP = corTreeLookup(entryP, "entities");

  if ((entitiesP == NULL) || (entitiesP->type != CorArray) || (entitiesP->value.head == NULL))
  {
    *whyP = "'entities' is missing - a non-empty array of the entities each record makes";
    return NULL;
  }

  RecordMap* mapP = (RecordMap*) calloc(1, sizeof(RecordMap));

  if (mapP == NULL)
  {
    *whyP = "out of memory";
    return NULL;
  }

  mapP->idEncoding   = idEncoding;
  mapP->otherColumns = otherColumns;

  RecordEntity* lastP = NULL;

  for (CorNode* itemP = entitiesP->value.head; itemP != NULL; itemP = itemP->next)
  {
    RecordEntity* entityP = entityParse(itemP, mapP, kaP, whyP);

    if (entityP == NULL)
    {
      recordMapFree(mapP);
      return NULL;
    }

    if (lastP == NULL)
      mapP->entities = entityP;
    else
      lastP->next = entityP;

    lastP = entityP;
  }

  //
  // The mapping as the file wrote it - for GET /dataChannels, which shows it back
  //
  mapP->text = nodeText(entitiesP);

  return mapP;
}



// -----------------------------------------------------------------------------
//
// recordMapFree -
//
void recordMapFree(RecordMap* mapP)
{
  if (mapP == NULL)
    return;

  RecordEntity* entityP = mapP->entities;

  while (entityP != NULL)
  {
    RecordEntity* nextP = entityP->next;
    entityFree(entityP);
    entityP = nextP;
  }

  for (int ix = 0; ix < mapP->columns; ix++)
    free(mapP->columnV[ix]);

  free(mapP->columnV);
  free(mapP->text);
  free(mapP);
}



// -----------------------------------------------------------------------------
//
// columnText - a column's value as text for a template; NULL when it has none
//
static const char* columnText(CorNode* recordP, const char* column, char* buf, int bufSize)
{
  CorNode* valueP = corTreeLookup(recordP, column);

  if (valueP == NULL)
    return NULL;

  switch (valueP->type)
  {
  case CorString:  return (valueP->value.s[0] != 0) ? valueP->value.s : NULL;
  case CorInt:     snprintf(buf, bufSize, "%lld", valueP->value.i);  return buf;
  case CorFloat:   snprintf(buf, bufSize, "%.15g", valueP->value.f); return buf;
  case CorBoolean: return (valueP->value.b == true) ? "true" : "false";
  default:         return NULL;
  }
}



// -----------------------------------------------------------------------------
//
// recordTemplateExpand -
//
char* recordTemplateExpand(const char* templ, CorNode* recordP, RecordIdEncoding idEncoding, CorAlloc* kaP)
{
  //
  // Two passes: the length, then the text
  //
  int   len = 0;
  char* out = NULL;

  for (int pass = 0; pass < 2; pass++)
  {
    int at = 0;

    for (const char* cP = templ; *cP != 0; cP++)
    {
      if (*cP != '{')
      {
        if (out != NULL)
          out[at] = *cP;
        at++;
        continue;
      }

      const char* endP = strchr(cP + 1, '}');     // there is one: the template was checked at load
      char        column[256];
      int         nameLen = endP - cP - 1;

      if (nameLen >= (int) sizeof(column))
        return NULL;

      memcpy(column, cP + 1, nameLen);
      column[nameLen] = 0;

      char        numBuf[64];
      const char* text = columnText(recordP, column, numBuf, sizeof(numBuf));

      if (text == NULL)
        return NULL;

      for (const char* tP = text; *tP != 0; tP++)
      {
        unsigned char c    = (unsigned char) *tP;
        bool          keep = ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) ||
                             (c == '.') || (c == '_') || (c == '-');

        if (keep == true)
        {
          if (out != NULL)
            out[at] = (char) c;
          at++;
        }
        else if (idEncoding == RecordIdPercent)     // each byte, %XX
        {
          if (out != NULL)
            snprintf(&out[at], 4, "%%%02X", c);
          at += 3;
        }
        else if ((c & 0xC0) != 0x80)                // merge: one '_' per character - the rest of a UTF-8 character adds none
        {
          if (out != NULL)
            out[at] = '_';
          at++;
        }
      }

      cP = endP;
    }

    if (pass == 0)
    {
      len = at;
      out = corAlloc(kaP, len + 1);

      if (out == NULL)
        return NULL;
    }
    else
      out[at] = 0;
  }

  return out;
}
