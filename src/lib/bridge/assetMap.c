//
// FILE            assetMap.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// An "assetChannels" entry -> AssetMap (AssetMap.h).
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // calloc, free, malloc
#include <string.h>                                   // strcmp, strdup

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corJson/corJsonRender.h"                    // corJsonFastRender
#include "corJson/corJsonRenderSize.h"                // corJsonFastRenderSize

#include "bridge/AssetMap.h"                          // AssetMap, AssetRule, AssetLayout
#include "bridge/assetMap.h"                          // Own interface



// -----------------------------------------------------------------------------
//
// stringMember - a member that, when there, is a non-empty string; *badP when it is there and is not
//
static const char* stringMember(CorNode* objectP, const char* name, bool* badP)
{
  CorNode* memberP = corTreeLookup(objectP, name);

  if (memberP == NULL)
    return NULL;

  if ((memberP->type != CorString) || (memberP->value.s[0] == 0))
  {
    *badP = true;
    return NULL;
  }

  return memberP->value.s;
}



// -----------------------------------------------------------------------------
//
// ruleFree -
//
static void ruleFree(AssetRule* ruleP)
{
  while (ruleP != NULL)
  {
    AssetRule* nextP = ruleP->next;

    free(ruleP->from);
    free(ruleP->typeDefinition);
    free(ruleP->semanticId);
    free(ruleP->type);
    free(ruleP);

    ruleP = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// ruleParse - one rule of "entities"
//
static AssetRule* ruleParse(CorNode* itemP, const char** whyP)
{
  static __thread char why[256];

  if (itemP->type != CorObject)
  {
    *whyP = "each rule of 'entities' is an object: { \"from\", \"type\" [, \"typeDefinition\"] [, \"semanticId\"] }";
    return NULL;
  }

  for (CorNode* memberP = itemP->value.head; memberP != NULL; memberP = memberP->next)
  {
    if ((strcmp(memberP->name, "from") != 0) && (strcmp(memberP->name, "type") != 0) &&
        (strcmp(memberP->name, "typeDefinition") != 0) && (strcmp(memberP->name, "semanticId") != 0))
    {
      snprintf(why, sizeof(why), "unknown member '%s' in a rule of 'entities' (a rule has 'from', 'type', 'typeDefinition' and 'semanticId')", memberP->name);
      *whyP = why;
      return NULL;
    }
  }

  bool        bad            = false;
  const char* from           = stringMember(itemP, "from", &bad);
  const char* type           = stringMember(itemP, "type", &bad);
  const char* typeDefinition = stringMember(itemP, "typeDefinition", &bad);
  const char* semanticId     = stringMember(itemP, "semanticId", &bad);

  if ((bad == true) || (from == NULL) || (type == NULL))
  {
    *whyP = "a rule of 'entities' names its node kind ('from') and its entity type ('type'), both non-empty strings - 'typeDefinition' and 'semanticId', when there, too";
    return NULL;
  }

  AssetRule* ruleP = (AssetRule*) calloc(1, sizeof(AssetRule));

  if (ruleP == NULL)
  {
    *whyP = "out of memory";
    return NULL;
  }

  ruleP->from           = strdup(from);
  ruleP->type           = strdup(type);
  ruleP->typeDefinition = (typeDefinition != NULL) ? strdup(typeDefinition) : NULL;
  ruleP->semanticId     = (semanticId     != NULL) ? strdup(semanticId)     : NULL;

  return ruleP;
}



// -----------------------------------------------------------------------------
//
// assetMapParse -
//
AssetMap* assetMapParse(CorNode* entryP, const char** whyP)
{
  static __thread char why[256];

  *whyP = NULL;

  if (entryP->type != CorObject)
  {
    *whyP = "the entry is not an object";
    return NULL;
  }

  for (CorNode* memberP = entryP->value.head; memberP != NULL; memberP = memberP->next)
  {
    if ((strcmp(memberP->name, "entities") != 0) && (strcmp(memberP->name, "attributeLayout") != 0) && (strcmp(memberP->name, "channelInfo") != 0))
    {
      snprintf(why, sizeof(why), "unknown member '%s' (an AssetChannel has 'entities', 'attributeLayout' and 'channelInfo')", memberP->name);
      *whyP = why;
      return NULL;
    }
  }

  //
  // attributeLayout - "attributes" unless the file says otherwise
  //
  AssetLayout layout  = AssetLayoutAttributes;
  CorNode*    layoutP = corTreeLookup(entryP, "attributeLayout");

  if (layoutP != NULL)
  {
    if ((layoutP->type == CorString) && (strcmp(layoutP->value.s, "attributes") == 0))
      layout = AssetLayoutAttributes;
    else if ((layoutP->type == CorString) && (strcmp(layoutP->value.s, "subAttributes") == 0))
      layout = AssetLayoutSubAttributes;
    else if ((layoutP->type == CorString) && (strcmp(layoutP->value.s, "value") == 0))
      layout = AssetLayoutValue;
    else
    {
      *whyP = "'attributeLayout' is \"attributes\" (the default), \"subAttributes\" or \"value\"";
      return NULL;
    }
  }

  //
  // entities - the data's owner says which nodes are entities: no default
  //
  CorNode* entitiesP = corTreeLookup(entryP, "entities");

  if (entitiesP == NULL)
  {
    *whyP = "'entities' is missing - \"natural\" (the source's own units) or the rules naming which nodes become entities: it has no default";
    return NULL;
  }

  bool natural = false;

  if (entitiesP->type == CorString)
  {
    if (strcmp(entitiesP->value.s, "natural") != 0)
    {
      *whyP = "'entities' is \"natural\" or an array of rules";
      return NULL;
    }

    natural = true;
  }
  else if ((entitiesP->type != CorArray) || (entitiesP->value.head == NULL))
  {
    *whyP = "'entities' is \"natural\" or a non-empty array of rules";
    return NULL;
  }

  AssetMap* mapP = (AssetMap*) calloc(1, sizeof(AssetMap));

  if (mapP == NULL)
  {
    *whyP = "out of memory";
    return NULL;
  }

  mapP->natural = natural;
  mapP->layout  = layout;

  AssetRule* lastP = NULL;

  for (CorNode* itemP = (natural == false) ? entitiesP->value.head : NULL; itemP != NULL; itemP = itemP->next)
  {
    AssetRule* ruleP = ruleParse(itemP, whyP);

    if (ruleP == NULL)
    {
      assetMapFree(mapP);
      return NULL;
    }

    if (lastP == NULL)
      mapP->rules = ruleP;
    else
      lastP->next = ruleP;

    lastP = ruleP;
  }

  //
  // "entities" as the file wrote it - for GET /assetChannels. Rendered detached: corJsonFastRender follows
  // the sibling chain and renders a name
  //
  char*    savedName = entitiesP->name;
  CorNode* savedNext = entitiesP->next;

  entitiesP->name = NULL;
  entitiesP->next = NULL;

  mapP->text = (char*) malloc(corJsonFastRenderSize(entitiesP) + 1);

  if (mapP->text != NULL)
    corJsonFastRender(entitiesP, mapP->text);

  entitiesP->name = savedName;
  entitiesP->next = savedNext;

  return mapP;
}



// -----------------------------------------------------------------------------
//
// assetMapFree -
//
void assetMapFree(AssetMap* mapP)
{
  if (mapP == NULL)
    return;

  ruleFree(mapP->rules);
  free(mapP->text);
  free(mapP);
}
