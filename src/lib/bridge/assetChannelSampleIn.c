//
// FILE            assetChannelSampleIn.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// An AssetChannel's model -> a graph of entities -> one batch upsert. See assetChannelSampleIn.h, and
// AssetChannel.h for the node tree.
//
// The mapping, per entity node:
//
//   its semanticIds                   the entity's "semanticId" Property
//   a leaf child (it has a "value")   a Property - "value", "unitCode" (unit), "observedAt" (sourceTime), a
//                                     "semanticId" sub-Property (its semanticIds)
//   a container child (not entity)    as attributeLayout says: its leaves attributes named by their path, or
//                                     one attribute and its leaves sub-attributes, or one JsonProperty
//   an entity child                   a Relationship to it, named by the child
//   a reference to an entity node     a Relationship, named by the reference's type (several: a
//                                     ListRelationship); to anything else a Property, the target's id
//   a method / operation              nothing yet - a service of the entity, later (doc/asset-channels.md § 4)
//
#include <ctype.h>                                    // isalnum, tolower, toupper
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // free, malloc
#include <string.h>                                   // strcmp, strlen, strchr

#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corAlloc/corAllocStrdup.h"                  // corAllocStrdup
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeArray, corTreeString, corTreeChildAdd, corTreeChildRemove
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corJson/corJsonRender.h"                    // corJsonFastRender
#include "corJson/corJsonRenderSize.h"                // corJsonFastRenderSize
#include "corLog/corLog.h"                            // COR_W, COR_T

#include "corRest/corRest.h"                          // corRest
#include "corBridge/BridgeBroker.h"                   // BRIDGE_OK, BRIDGE_BAD_INPUT, BRIDGE_ERR
#include "corJsonld/corLdExpandTree.h"                // corLdExpandEntityTree
#include "corNgsild/CorNgsild.h"                      // ldDefaultContext
#include "corNgsild/ldHooks.h"                        // corNgsildFallbackRelease

#include "bridge/AssetChannel.h"                      // AssetChannel
#include "bridge/AssetMap.h"                          // AssetMap, AssetRule, AssetLayout
#include "bridge/bridgeSampleIn.h"                    // bridgeThreadBind
#include "bridge/bridgeEntitiesUpsert.h"              // bridgeEntitiesUpsert
#include "bridge/assetChannelSampleIn.h"              // Own interface
#include "coraineTraceLevels.h"                       // CtBridge



// -----------------------------------------------------------------------------
//
// AssetEntity - a node of the model that is an entity
//
typedef struct AssetEntity
{
  CorNode*  nodeP;
  char*     id;
  char*     type;
  bool      write;                                    // written by this sample
} AssetEntity;



// -----------------------------------------------------------------------------
//
// AssetModel - the model of one sample, its entities found
//
typedef struct AssetModel
{
  AssetMap*     mapP;
  AssetEntity*  entityV;
  int           entities;
  int           entitiesMax;
} AssetModel;



// -----------------------------------------------------------------------------
//
// str - a string member of a node; NULL when absent or not a string
//
static const char* str(CorNode* nodeP, const char* name)
{
  CorNode* memberP = corTreeLookup(nodeP, name);

  return ((memberP != NULL) && (memberP->type == CorString)) ? memberP->value.s : NULL;
}



// -----------------------------------------------------------------------------
//
// isIri - the source's id is an IRI, and so already an NGSI-LD id: "<scheme>:<rest>" (an AAS id is; an OPC UA
// NodeId, "ns=2;s=Press17", is not)
//
static bool isIri(const char* id)
{
  if ((id == NULL) || (isalpha((unsigned char) id[0]) == 0))
    return false;

  for (const char* p = id + 1; *p != 0; p++)
  {
    if (*p == ':')
      return p[1] != 0;

    if ((isalnum((unsigned char) *p) == 0) && (*p != '+') && (*p != '-') && (*p != '.'))
      return false;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// hasSemanticId - is 'id' one of the node's semanticIds?
//
static bool hasSemanticId(CorNode* nodeP, const char* id)
{
  CorNode* idsP = corTreeLookup(nodeP, "semanticIds");

  for (CorNode* idP = ((idsP != NULL) && (idsP->type == CorArray)) ? idsP->value.head : NULL; idP != NULL; idP = idP->next)
  {
    if ((idP->type == CorString) && (strcmp(idP->value.s, id) == 0))
      return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// entityTypeOf - the entity type of a node the mapping makes an entity; NULL: not an entity
//
static const char* entityTypeOf(AssetMap* mapP, CorNode* nodeP)
{
  const char* kind = str(nodeP, "node");

  if (kind == NULL)
    return NULL;

  if (mapP->natural == true)
  {
    if ((strcmp(kind, "object") != 0) && (strcmp(kind, "shell") != 0) && (strcmp(kind, "submodel") != 0))
      return NULL;

    const char* typeName = str(nodeP, "typeName");

    if (typeName != NULL)
      return typeName;

    char* type = corAllocStrdup(&corRest.kalloc, kind);

    if (type != NULL)
      type[0] = (char) toupper((unsigned char) type[0]);

    return type;
  }

  for (AssetRule* ruleP = mapP->rules; ruleP != NULL; ruleP = ruleP->next)
  {
    if (strcmp(ruleP->from, kind) != 0)
      continue;

    if (ruleP->typeDefinition != NULL)
    {
      const char* typeDefinition = str(nodeP, "typeDefinition");

      if ((typeDefinition == NULL) || (strcmp(typeDefinition, ruleP->typeDefinition) != 0))
        continue;
    }

    if ((ruleP->semanticId != NULL) && (hasSemanticId(nodeP, ruleP->semanticId) == false))
      continue;

    return ruleP->type;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// idSegmentAppend - a node's name (else its id) to an entity id, each byte outside [A-Za-z0-9._-] as %XX
//
static void idSegmentAppend(char* buf, int bufSize, CorNode* nodeP)
{
  const char* name = str(nodeP, "name");

  if (name == NULL)
    name = str(nodeP, "id");
  if (name == NULL)
    name = "_";

  int len = strlen(buf);

  for (const unsigned char* p = (const unsigned char*) name; (*p != 0) && (len < bufSize - 4); p++)
  {
    if ((isalnum(*p) != 0) || (*p == '.') || (*p == '_') || (*p == '-'))
      buf[len++] = (char) *p;
    else
      len += snprintf(&buf[len], 4, "%%%02X", *p);
  }

  buf[len] = 0;
}



// -----------------------------------------------------------------------------
//
// entitiesFind - every node of the tree the mapping makes an entity, with its id
//
// The id: the node's own when it is an IRI; else urn:ngsi-ld:<type>:<the names from the root to the node>.
//
static void entitiesFind(AssetModel* modelP, CorNode* nodeP, char* path, int pathSize)
{
  int         pathLen = strlen(path);
  const char* type    = entityTypeOf(modelP->mapP, nodeP);

  if (pathLen > 0)
    strncat(path, ":", pathSize - pathLen - 1);

  idSegmentAppend(path, pathSize, nodeP);

  if ((type != NULL) && (modelP->entities < modelP->entitiesMax))
  {
    const char*  sourceId = str(nodeP, "id");
    AssetEntity* entP     = &modelP->entityV[modelP->entities++];

    entP->nodeP = nodeP;
    entP->type  = (char*) type;
    entP->write = true;

    if (isIri(sourceId) == true)
      entP->id = (char*) sourceId;
    else
    {
      int len = 12 + strlen(type) + 1 + strlen(path) + 1;   // "urn:ngsi-ld:" = 12

      entP->id = (char*) corAlloc(&corRest.kalloc, len);

      if (entP->id != NULL)
        snprintf(entP->id, len, "urn:ngsi-ld:%s:%s", type, path);
    }
  }

  CorNode* childrenP = corTreeLookup(nodeP, "children");

  for (CorNode* childP = ((childrenP != NULL) && (childrenP->type == CorArray)) ? childrenP->value.head : NULL; childP != NULL; childP = childP->next)
  {
    if (childP->type == CorObject)
      entitiesFind(modelP, childP, path, pathSize);
  }

  path[pathLen] = 0;
}



// -----------------------------------------------------------------------------
//
// entityOfNode / entityOfSourceId - the entity a node is, or the entity node whose source id is that; NULL
//
static AssetEntity* entityOfNode(AssetModel* modelP, CorNode* nodeP)
{
  for (int ix = 0; ix < modelP->entities; ix++)
  {
    if (modelP->entityV[ix].nodeP == nodeP)
      return &modelP->entityV[ix];
  }

  return NULL;
}

static AssetEntity* entityOfSourceId(AssetModel* modelP, const char* sourceId)
{
  for (int ix = 0; ix < modelP->entities; ix++)
  {
    const char* id = str(modelP->entityV[ix].nodeP, "id");

    if ((id != NULL) && (strcmp(id, sourceId) == 0))
      return &modelP->entityV[ix];
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// attrName - an attribute name from a path: "Hydraulics" + "Oil Temp" -> hydraulicsOilTemp
//
// Only [A-Za-z0-9_] are kept; the first letter lower-case, the first of each further segment upper-case.
// NULL when nothing is left, or when the name is one an entity has already (id, type).
//
static char* attrName(const char* prefix, const char* name)
{
  if (name == NULL)
    return NULL;

  int   len = ((prefix != NULL) ? strlen(prefix) : 0) + strlen(name) + 1;
  char* out = (char*) corAlloc(&corRest.kalloc, len);

  if (out == NULL)
    return NULL;

  int  n     = 0;
  bool upper = false;

  if (prefix != NULL)
  {
    strcpy(out, prefix);
    n     = strlen(prefix);
    upper = (n > 0);
  }

  for (const unsigned char* p = (const unsigned char*) name; *p != 0; p++)
  {
    if ((isalnum(*p) == 0) && (*p != '_'))
    {
      upper = (n > 0);                                // "Oil Temp": the T of Temp starts a word
      continue;
    }

    if (n == 0)
      out[n++] = (char) tolower(*p);
    else if (upper == true)
      out[n++] = (char) toupper(*p);
    else
      out[n++] = (char) *p;

    upper = false;
  }

  out[n] = 0;

  if ((n == 0) || (strcmp(out, "id") == 0) || (strcmp(out, "type") == 0))
    return NULL;

  return out;
}



// -----------------------------------------------------------------------------
//
// semanticIdNode - the node's semanticIds as a "semanticId" Property (one: a string, several: an array); NULL
// when it has none
//
static CorNode* semanticIdNode(CorNode* nodeP)
{
  CorNode* idsP = corTreeLookup(nodeP, "semanticIds");

  if ((idsP == NULL) || (idsP->type != CorArray) || (idsP->value.head == NULL))
    return NULL;

  CorNode* propP  = corTreeObject(corRest.kallocP, "semanticId");
  CorNode* valueP = (idsP->value.head->next == NULL) ? corTreeClone(corRest.kallocP, idsP->value.head) : corTreeClone(corRest.kallocP, idsP);

  if ((propP == NULL) || (valueP == NULL))
    return NULL;

  valueP->name = (char*) "value";
  valueP->next = NULL;

  corTreeChildAdd(propP, corTreeString(corRest.kallocP, "type", "Property"));
  corTreeChildAdd(propP, valueP);

  return propP;
}



// -----------------------------------------------------------------------------
//
// isLeaf / isMethod -
//
static bool isLeaf(CorNode* nodeP)
{
  return corTreeLookup(nodeP, "value") != NULL;
}

static bool isMethod(CorNode* nodeP)
{
  const char* kind = str(nodeP, "node");

  return (kind != NULL) && ((strcmp(kind, "method") == 0) || (strcmp(kind, "operation") == 0));
}



// -----------------------------------------------------------------------------
//
// leafProperty - a leaf as a Property named 'name': value, unitCode, observedAt, semanticId
//
static CorNode* leafProperty(CorNode* leafP, const char* name)
{
  CorNode* valueP = corTreeClone(corRest.kallocP, corTreeLookup(leafP, "value"));
  CorNode* propP  = corTreeObject(corRest.kallocP, name);

  if ((valueP == NULL) || (propP == NULL))
    return NULL;

  valueP->name = (char*) "value";
  valueP->next = NULL;

  corTreeChildAdd(propP, corTreeString(corRest.kallocP, "type", "Property"));
  corTreeChildAdd(propP, valueP);

  const char* unit       = str(leafP, "unit");
  const char* sourceTime = str(leafP, "sourceTime");
  CorNode*    semanticP  = semanticIdNode(leafP);

  if (unit != NULL)
    corTreeChildAdd(propP, corTreeString(corRest.kallocP, "unitCode", (char*) unit));

  if (sourceTime != NULL)
    corTreeChildAdd(propP, corTreeString(corRest.kallocP, "observedAt", (char*) sourceTime));

  if (semanticP != NULL)
    corTreeChildAdd(propP, semanticP);

  return propP;
}



// -----------------------------------------------------------------------------
//
// plainJson - a container as the source has it: { "<name>": <value> | { ... } }
//
static CorNode* plainJson(CorNode* containerP, const char* name)
{
  CorNode* objectP   = corTreeObject(corRest.kallocP, name);
  CorNode* childrenP = corTreeLookup(containerP, "children");

  for (CorNode* childP = ((childrenP != NULL) && (childrenP->type == CorArray)) ? childrenP->value.head : NULL; childP != NULL; childP = childP->next)
  {
    const char* childName = str(childP, "name");

    if ((childName == NULL) || (corTreeLookup(objectP, childName) != NULL) || (isMethod(childP) == true))
      continue;

    if (isLeaf(childP) == true)
    {
      CorNode* valueP = corTreeClone(corRest.kallocP, corTreeLookup(childP, "value"));

      if (valueP != NULL)
      {
        valueP->name = (char*) childName;
        valueP->next = NULL;
        corTreeChildAdd(objectP, valueP);
      }
    }
    else
      corTreeChildAdd(objectP, plainJson(childP, childName));
  }

  return objectP;
}



// -----------------------------------------------------------------------------
//
// attrAdd - an attribute to the entity, unless it has one of that name already (the first one stays)
//
static void attrAdd(CorNode* entityP, CorNode* attrP)
{
  if ((attrP == NULL) || (attrP->name == NULL) || (corTreeLookup(entityP, attrP->name) != NULL))
    return;

  corTreeChildAdd(entityP, attrP);
}



// -----------------------------------------------------------------------------
//
// relationshipAdd - a Relationship 'name' to 'object'; a second one of the same name makes it a
// ListRelationship
//
static void relationshipAdd(CorNode* entityP, const char* name, const char* object)
{
  if (name == NULL)
    return;

  CorNode* attrP = corTreeLookup(entityP, name);

  if (attrP == NULL)
  {
    attrP = corTreeObject(corRest.kallocP, name);
    corTreeChildAdd(attrP, corTreeString(corRest.kallocP, "type", "Relationship"));
    corTreeChildAdd(attrP, corTreeString(corRest.kallocP, "object", (char*) object));
    corTreeChildAdd(entityP, attrP);
    return;
  }

  CorNode* typeP = corTreeLookup(attrP, "type");

  if ((typeP == NULL) || (typeP->type != CorString))
    return;

  if (strcmp(typeP->value.s, "Relationship") == 0)
  {
    CorNode* objectP = corTreeLookup(attrP, "object");
    CorNode* listP   = corTreeArray(corRest.kallocP, "objectList");

    if ((objectP == NULL) || (listP == NULL))
      return;

    corTreeChildRemove(attrP, objectP);
    objectP->name = NULL;
    corTreeChildAdd(listP, objectP);
    corTreeChildAdd(attrP, listP);
    typeP->value.s = (char*) "ListRelationship";
  }

  if (strcmp(typeP->value.s, "ListRelationship") == 0)
    corTreeChildAdd(corTreeLookup(attrP, "objectList"), corTreeString(corRest.kallocP, NULL, (char*) object));
}



// -----------------------------------------------------------------------------
//
// referencesMap - a node's references, as attributes of the entity it is in
//
static void referencesMap(AssetModel* modelP, CorNode* entityP, CorNode* nodeP)
{
  CorNode* refsP = corTreeLookup(nodeP, "references");

  for (CorNode* refP = ((refsP != NULL) && (refsP->type == CorArray)) ? refsP->value.head : NULL; refP != NULL; refP = refP->next)
  {
    const char* refType = str(refP, "type");
    const char* target  = str(refP, "target");
    char*       name    = attrName(NULL, refType);

    if ((name == NULL) || (target == NULL))
      continue;

    AssetEntity* targetP = entityOfSourceId(modelP, target);

    if (targetP != NULL)
      relationshipAdd(entityP, name, targetP->id);
    else
    {
      CorNode* propP = corTreeObject(corRest.kallocP, name);

      corTreeChildAdd(propP, corTreeString(corRest.kallocP, "type", "Property"));
      corTreeChildAdd(propP, corTreeString(corRest.kallocP, "value", (char*) target));
      attrAdd(entityP, propP);
    }
  }
}



// -----------------------------------------------------------------------------
//
// childrenMap - the children of an entity node (or of a container in it, 'prefix' its path), as attributes;
// a container that is not an entity as 'layout' says
//
static void childrenMap(AssetModel* modelP, CorNode* entityP, CorNode* nodeP, const char* prefix, AssetLayout layout)
{
  CorNode* childrenP = corTreeLookup(nodeP, "children");

  for (CorNode* childP = ((childrenP != NULL) && (childrenP->type == CorArray)) ? childrenP->value.head : NULL; childP != NULL; childP = childP->next)
  {
    if ((childP->type != CorObject) || (isMethod(childP) == true))
      continue;

    char*        name         = attrName(prefix, str(childP, "name"));
    AssetEntity* entityChildP = entityOfNode(modelP, childP);

    if (entityChildP != NULL)
    {
      relationshipAdd(entityP, name, entityChildP->id);
      continue;
    }

    if (name == NULL)
      continue;

    if (isLeaf(childP) == true)
    {
      attrAdd(entityP, leafProperty(childP, name));
      referencesMap(modelP, entityP, childP);
      continue;
    }

    //
    // A container that is not an entity - as attributeLayout says
    //
    switch (layout)
    {
    case AssetLayoutAttributes:
      childrenMap(modelP, entityP, childP, name, AssetLayoutAttributes);
      break;

    case AssetLayoutSubAttributes:
    {
      CorNode*    propP = corTreeObject(corRest.kallocP, name);
      const char* label = str(childP, "name");

      corTreeChildAdd(propP, corTreeString(corRest.kallocP, "type", "Property"));
      corTreeChildAdd(propP, corTreeString(corRest.kallocP, "value", (char*) label));
      childrenMap(modelP, propP, childP, NULL, AssetLayoutAttributes);   // one level: a deeper container's leaves by their path
      attrAdd(entityP, propP);
      break;
    }

    case AssetLayoutValue:
    {
      CorNode* propP = corTreeObject(corRest.kallocP, name);
      CorNode* jsonP = plainJson(childP, "json");

      corTreeChildAdd(propP, corTreeString(corRest.kallocP, "type", "JsonProperty"));
      corTreeChildAdd(propP, jsonP);
      attrAdd(entityP, propP);
      break;
    }
    }

    referencesMap(modelP, entityP, childP);
  }
}



// -----------------------------------------------------------------------------
//
// entityMake - one entity node as an NGSI-LD entity
//
static CorNode* entityMake(AssetModel* modelP, AssetEntity* entP)
{
  CorNode* entityP = corTreeObject(corRest.kallocP, NULL);

  corTreeChildAdd(entityP, corTreeString(corRest.kallocP, "id",   entP->id));
  corTreeChildAdd(entityP, corTreeString(corRest.kallocP, "type", entP->type));

  attrAdd(entityP, semanticIdNode(entP->nodeP));
  childrenMap(modelP, entityP, entP->nodeP, NULL, modelP->mapP->layout);
  referencesMap(modelP, entityP, entP->nodeP);

  return entityP;
}



// -----------------------------------------------------------------------------
//
// nodeCount - the nodes of a tree: the most entities it can make
//
static int nodeCount(CorNode* nodeP)
{
  int      n         = 1;
  CorNode* childrenP = corTreeLookup(nodeP, "children");

  for (CorNode* childP = ((childrenP != NULL) && (childrenP->type == CorArray)) ? childrenP->value.head : NULL; childP != NULL; childP = childP->next)
    n += nodeCount(childP);

  return n;
}



// -----------------------------------------------------------------------------
//
// nodeById - the node of the tree whose "id" is 'id', and the entity node it is in (the nearest one above it,
// or itself)
//
static CorNode* nodeById(AssetModel* modelP, CorNode* nodeP, const char* id, AssetEntity* inP, AssetEntity** entityPP)
{
  AssetEntity* selfP = entityOfNode(modelP, nodeP);

  if (selfP != NULL)
    inP = selfP;

  const char* nodeId = str(nodeP, "id");

  if ((nodeId != NULL) && (strcmp(nodeId, id) == 0))
  {
    *entityPP = inP;
    return nodeP;
  }

  CorNode* childrenP = corTreeLookup(nodeP, "children");

  for (CorNode* childP = ((childrenP != NULL) && (childrenP->type == CorArray)) ? childrenP->value.head : NULL; childP != NULL; childP = childP->next)
  {
    CorNode* foundP = nodeById(modelP, childP, id, inP, entityPP);

    if (foundP != NULL)
      return foundP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// memberSet - a node's member replaced by a clone of 'valueP' (named 'name')
//
static void memberSet(CorNode* nodeP, const char* name, CorNode* valueP)
{
  CorNode* oldP = corTreeLookup(nodeP, name);

  if (oldP != NULL)
    corTreeChildRemove(nodeP, oldP);

  CorNode* newP = corTreeClone(corRest.kallocP, valueP);

  if (newP == NULL)
    return;

  newP->name = (char*) name;
  newP->next = NULL;
  corTreeChildAdd(nodeP, newP);
}



// -----------------------------------------------------------------------------
//
// modelKeep - the model as JSON text, the AssetChannel's (malloc) - in place of what it had. Under modelMutex
//
static void modelKeep(AssetChannel* acP, CorNode* modelP)
{
  char*    savedName = modelP->name;
  CorNode* savedNext = modelP->next;

  modelP->name = NULL;                                // rendered detached: corJsonFastRender follows the sibling chain
  modelP->next = NULL;

  char* text = (char*) malloc(corJsonFastRenderSize(modelP) + 1);

  if (text != NULL)
  {
    corJsonFastRender(modelP, text);
    free(acP->model);
    acP->model = text;
  }

  modelP->name = savedName;
  modelP->next = savedNext;
}



// -----------------------------------------------------------------------------
//
// write - the entities of the model marked 'write', as one batch upsert; how many were written
//
static int write(AssetChannel* acP, AssetModel* modelP)
{
  CorNode* entitiesP = corTreeArray(corRest.kallocP, NULL);
  int      entities  = 0;

  for (int ix = 0; ix < modelP->entities; ix++)
  {
    if ((modelP->entityV[ix].write == false) || (modelP->entityV[ix].id == NULL))
      continue;

    corTreeChildAdd(entitiesP, entityMake(modelP, &modelP->entityV[ix]));
    entities++;
  }

  if (entities == 0)
  {
    corNgsildFallbackRelease();                       // no upsert to release it
    return 0;
  }

  corLdExpandEntityTree(entitiesP, ldDefaultContext(&corRest.kalloc), &corRest.kalloc);

  char what[256];

  snprintf(what, sizeof(what), "model on AssetChannel '%s'", acP->endpoint);

  return bridgeEntitiesUpsert(entitiesP, entities, what);
}



// -----------------------------------------------------------------------------
//
// modelFind - the model's entities, found
//
static bool modelFind(AssetModel* modelP, AssetMap* mapP, CorNode* treeP)
{
  char path[1024];

  path[0]             = 0;
  modelP->mapP        = mapP;
  modelP->entities    = 0;
  modelP->entitiesMax = nodeCount(treeP);
  modelP->entityV     = (AssetEntity*) corAlloc(&corRest.kalloc, modelP->entitiesMax * sizeof(AssetEntity));

  if (modelP->entityV == NULL)
    return false;

  entitiesFind(modelP, treeP, path, sizeof(path));
  return true;
}



// -----------------------------------------------------------------------------
//
// modelIn - { "model": <node> }
//
static int modelIn(AssetChannel* acP, CorNode* treeP)
{
  AssetModel model;

  if ((treeP->type != CorObject) || (modelFind(&model, acP->mapP, treeP) == false) || (model.entities == 0))
  {
    COR_W("bridge '%s': the model on AssetChannel '%s' makes no entity - dropped", acP->bridgeName, acP->endpoint);
    __atomic_add_fetch(&acP->samplesRefused, 1, __ATOMIC_RELAXED);
    corNgsildFallbackRelease();
    return BRIDGE_BAD_INPUT;
  }

  pthread_mutex_lock(&acP->modelMutex);
  modelKeep(acP, treeP);
  pthread_mutex_unlock(&acP->modelMutex);

  int written = write(acP, &model);

  if (written == 0)
    return BRIDGE_ERR;

  __atomic_add_fetch(&acP->modelsIn, 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&acP->entitiesWritten, (uint64_t) written, __ATOMIC_RELAXED);
  COR_T(CtBridge, "model on AssetChannel '%s' -> %d entit%s", acP->endpoint, written, (written == 1) ? "y" : "ies");

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// valuesIn - { "values": [ { "id", "value", "sourceTime" }, ... ] }
//
// The kept model is updated under modelMutex - two samples of values must not each start from the model
// before the other - and the entities the changed nodes are in are written again, from the updated model.
//
static int valuesIn(AssetChannel* acP, CorNode* valuesP)
{
  int count = 0;

  for (CorNode* valueP = valuesP->value.head; valueP != NULL; valueP = valueP->next)
    count++;

  pthread_mutex_lock(&acP->modelMutex);

  char*    text  = (acP->model != NULL) ? corAllocStrdup(&corRest.kalloc, acP->model) : NULL;
  CorNode* treeP = (text != NULL) ? corJsonParse(corRest.corJsonP, text) : NULL;

  AssetModel model;

  if ((treeP == NULL) || (modelFind(&model, acP->mapP, treeP) == false))
  {
    pthread_mutex_unlock(&acP->modelMutex);
    COR_W("bridge '%s': values on AssetChannel '%s' before its model - dropped", acP->bridgeName, acP->endpoint);
    __atomic_add_fetch(&acP->valuesRefused, (uint64_t) count, __ATOMIC_RELAXED);
    corNgsildFallbackRelease();
    return BRIDGE_BAD_INPUT;
  }

  for (int ix = 0; ix < model.entities; ix++)
    model.entityV[ix].write = false;

  int set = 0;

  for (CorNode* itemP = valuesP->value.head; itemP != NULL; itemP = itemP->next)
  {
    const char*  id     = (itemP->type == CorObject) ? str(itemP, "id") : NULL;
    CorNode*     newP   = (itemP->type == CorObject) ? corTreeLookup(itemP, "value") : NULL;
    AssetEntity* inP    = NULL;
    CorNode*     nodeP  = ((id != NULL) && (newP != NULL)) ? nodeById(&model, treeP, id, NULL, &inP) : NULL;

    if ((nodeP == NULL) || (inP == NULL))
    {
      __atomic_add_fetch(&acP->valuesRefused, 1, __ATOMIC_RELAXED);
      COR_T(CtBridge, "AssetChannel '%s': a value of no node of the model ('%s') - dropped", acP->endpoint, (id != NULL) ? id : "?");
      continue;
    }

    memberSet(nodeP, "value", newP);

    CorNode* sourceTimeP = corTreeLookup(itemP, "sourceTime");

    if (sourceTimeP != NULL)
      memberSet(nodeP, "sourceTime", sourceTimeP);

    inP->write = true;
    set++;
  }

  if (set > 0)
    modelKeep(acP, treeP);

  pthread_mutex_unlock(&acP->modelMutex);

  if (set == 0)
  {
    corNgsildFallbackRelease();
    return BRIDGE_BAD_INPUT;
  }

  int written = write(acP, &model);

  if (written == 0)
    return BRIDGE_ERR;

  __atomic_add_fetch(&acP->valuesIn, (uint64_t) set, __ATOMIC_RELAXED);
  __atomic_add_fetch(&acP->entitiesWritten, (uint64_t) written, __ATOMIC_RELAXED);
  COR_T(CtBridge, "%d value%s on AssetChannel '%s' -> %d entit%s", set, (set == 1) ? "" : "s", acP->endpoint, written, (written == 1) ? "y" : "ies");

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// assetChannelSampleIn -
//
int assetChannelSampleIn(AssetChannel* acP, const char* json, int64_t publishTime)
{
  (void) publishTime;

  bridgeThreadBind(acP->tenantP);

  char*    text    = corAllocStrdup(&corRest.kalloc, json);
  CorNode* sampleP = (text != NULL) ? corJsonParse(corRest.corJsonP, text) : NULL;
  CorNode* modelP  = ((sampleP != NULL) && (sampleP->type == CorObject)) ? corTreeLookup(sampleP, "model")  : NULL;
  CorNode* valuesP = ((sampleP != NULL) && (sampleP->type == CorObject)) ? corTreeLookup(sampleP, "values") : NULL;
  int      r;

  if (modelP != NULL)
    r = modelIn(acP, modelP);
  else if ((valuesP != NULL) && (valuesP->type == CorArray))
    r = valuesIn(acP, valuesP);
  else
  {
    COR_W("bridge '%s': the sample on AssetChannel '%s' is neither { \"model\": ... } nor { \"values\": [ ... ] } - dropped", acP->bridgeName, acP->endpoint);
    __atomic_add_fetch(&acP->samplesRefused, 1, __ATOMIC_RELAXED);
    corNgsildFallbackRelease();
    r = BRIDGE_BAD_INPUT;
  }

  return r;
}
