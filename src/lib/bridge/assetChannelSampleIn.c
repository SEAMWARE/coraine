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
//                                     "semanticId" sub-Property (its semanticIds), a "contentType" one;
//                                     its attributeType LanguageProperty (a language map) or ListProperty
//                                     (an array): that type, the value its languageMap or valueList
//   a container child (not entity)    as attributeLayout says: its leaves attributes named by their path, or
//                                     one attribute and its leaves sub-attributes, or one JsonProperty
//   an entity child                   a Relationship to it, named by the child
//   a reference                       to a node that is an entity, or is in one - of this model or of another
//                                     AssetChannel's: a Relationship to that entity, named by the reference's
//                                     type (several: a ListRelationship); to a node in it, not the entity
//                                     itself: a "targetNode" sub-Property, the node's id (a ListRelationship:
//                                     a ListProperty, one per object). To anything else a Property, the
//                                     target's id. A new model on the other AssetChannel maps this one again.
//   a method / operation              no attribute - a Service Registration of the entity, the operation's
//                                     endpoint (assetChannelServices.c, COR_FEATURE_ASSET_CHANNEL_SERVICES)
//
#include <ctype.h>                                    // isalnum, isupper, islower, tolower, toupper
#include <pthread.h>                                  // pthread_mutex_lock, pthread_mutex_unlock
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
#include "corJsonld/corLdExpand.h"                    // corLdExpand
#include "corNgsild/CorNgsild.h"                      // ldDefaultContext
#include "corNgsild/ldHooks.h"                        // corNgsildFallbackRelease

#include "bridge/AssetChannel.h"                      // AssetChannel
#include "bridge/AssetMap.h"                          // AssetMap, AssetRule, AssetLayout
#include "bridge/bridgeSampleIn.h"                    // bridgeThreadBind
#include "bridge/bridgeEntitiesUpsert.h"              // bridgeEntitiesUpsert
#include "bridge/assetChannelCache.h"                 // assetChannelMappedFree, assetChannelStatusSet
#if COR_FEATURE_ASSET_CHANNEL_SERVICES
#include "bridge/assetChannelServices.h"              // AssetOperation, assetChannelServicesSync
#endif
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
typedef struct AssetRefAttr
{
  CorNode*     holderP;                               // the entity, or the attribute, the reference is in
  char*        name;                                  // NULL: made with an earlier one of its name
  const char*  target;
  const char*  object;                                // the entity; NULL: none - a Property
  bool         inside;                                // the target is a node in the entity, not the entity
} AssetRefAttr;

typedef struct AssetModel
{
  AssetChannel*    acP;
  AssetMap*        mapP;
  CorNode*         rootP;                             // the tree: a reference's target is looked up in it first
  AssetEntity*     entityV;
  int              entities;
  int              entitiesMax;
  AssetNode*       nodeV;                             // the nodes in entities - kept, for the other AssetChannels' references
  int              nodes;
  AssetReference*  refV;                              // the references to other AssetChannels' nodes, what they became
  int              refs;
  int              refsMax;
  AssetRefAttr*    refAttrV;                          // the entity being made: its references, made at its end
  int              refAttrs;
#if COR_FEATURE_CHANNEL_CHECKS || COR_FEATURE_CHANNEL_WRITE_BACK
  CorNode*      curEntityP;                           // the entity being made: its attributes are recorded
  const char*   curEntityId;
  AssetMapped*  mappedV;                              // what the entities made have - in the request's arena, names compact
  int           mapped;
  int           mappedMax;
#endif
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
// entityOfNode - the entity a node is; NULL
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
    {
      //
      // The name's first word in lower case - a leading acronym whole: URIOfTheProduct -> uriOfTheProduct,
      // AASReference -> aasReference, PLC -> plc; its last capital starts the next word when a lower-case
      // letter follows it (the O of Of)
      //
      const unsigned char* q = p;

      while ((isupper(*p) != 0) && (isupper(q[1]) != 0))
        q++;

      if ((q > p) && (islower(q[1]) != 0))
        q--;

      for (; p <= q; p++)
        out[n++] = (char) tolower(*p);

      p--;
    }
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
static bool languageMapIs(CorNode* valueP)
{
  if ((valueP->type != CorObject) || (valueP->value.head == NULL))
    return false;

  for (CorNode* langP = valueP->value.head; langP != NULL; langP = langP->next)
  {
    if (langP->type != CorString)
      return false;
  }

  return true;
}

static CorNode* leafProperty(CorNode* leafP, const char* name)
{
  CorNode* valueP = corTreeClone(corRest.kallocP, corTreeLookup(leafP, "value"));
  CorNode* propP  = corTreeObject(corRest.kallocP, name);

  if ((valueP == NULL) || (propP == NULL))
    return NULL;

  //
  // The leaf's attributeType: a LanguageProperty (its value a language map - an AAS MultiLanguageProperty)
  // or a ListProperty (an array - an AAS SubmodelElementList); a value that is not one is a Property
  //
  const char* attrType = str(leafP, "attributeType");
  const char* type     = "Property";
  const char* member   = "value";

  if ((attrType != NULL) && (strcmp(attrType, "LanguageProperty") == 0) && (languageMapIs(valueP) == true))
  {
    type   = "LanguageProperty";
    member = "languageMap";
  }
  else if ((attrType != NULL) && (strcmp(attrType, "ListProperty") == 0) && (valueP->type == CorArray))
  {
    type   = "ListProperty";
    member = "valueList";
  }

  valueP->name = (char*) member;
  valueP->next = NULL;

  corTreeChildAdd(propP, corTreeString(corRest.kallocP, "type", (char*) type));
  corTreeChildAdd(propP, valueP);

  const char* unit        = str(leafP, "unit");
  const char* contentType = str(leafP, "contentType");
  const char* sourceTime  = str(leafP, "sourceTime");
  CorNode*    semanticP   = semanticIdNode(leafP);

  if (unit != NULL)
    corTreeChildAdd(propP, corTreeString(corRest.kallocP, "unitCode", (char*) unit));

  if (contentType != NULL)                            // an AAS File's, a Blob's: a sub-Property
  {
    CorNode* ctP = corTreeObject(corRest.kallocP, "contentType");

    corTreeChildAdd(ctP, corTreeString(corRest.kallocP, "type",  "Property"));
    corTreeChildAdd(ctP, corTreeString(corRest.kallocP, "value", (char*) contentType));
    corTreeChildAdd(propP, ctP);
  }

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



#if COR_FEATURE_CHANNEL_CHECKS || COR_FEATURE_CHANNEL_WRITE_BACK
// -----------------------------------------------------------------------------
//
// mappedAdd - an attribute of the entity being made, and the node it comes from (NULL: none) - the first
// of a name stays, as attrAdd keeps it
//
static void mappedAdd(AssetModel* modelP, const char* attrName, const char* nodeId)
{
  for (int ix = 0; ix < modelP->mapped; ix++)
  {
    if ((modelP->mappedV[ix].entityId == modelP->curEntityId) && (strcmp(modelP->mappedV[ix].attrName, attrName) == 0))
      return;
  }

  if (modelP->mapped == modelP->mappedMax)
  {
    int          max = (modelP->mappedMax == 0) ? 64 : modelP->mappedMax * 2;
    AssetMapped* v   = (AssetMapped*) corAlloc(&corRest.kalloc, max * sizeof(AssetMapped));

    if (v == NULL)
      return;

    if (modelP->mapped > 0)
      memcpy(v, modelP->mappedV, modelP->mapped * sizeof(AssetMapped));

    modelP->mappedV   = v;
    modelP->mappedMax = max;
  }

  modelP->mappedV[modelP->mapped].entityId = (char*) modelP->curEntityId;
  modelP->mappedV[modelP->mapped].attrName = (char*) attrName;
  modelP->mappedV[modelP->mapped].nodeId   = (char*) nodeId;
  modelP->mapped++;
}



// -----------------------------------------------------------------------------
//
// mappedKeep - what the model made, the AssetChannel's (malloc, names expanded) - in place of what it had
//
static void mappedKeep(AssetChannel* acP, AssetModel* modelP)
{
  AssetMapped* keptV = (AssetMapped*) calloc(modelP->mapped + 1, sizeof(AssetMapped));
  int          kept  = 0;

  for (int ix = 0; (keptV != NULL) && (ix < modelP->mapped); ix++)
  {
    char* expanded = corLdExpand(ldDefaultContext(&corRest.kalloc), modelP->mappedV[ix].attrName, &corRest.kalloc, NULL, NULL);

    if (expanded == NULL)
      continue;

    keptV[kept].entityId = strdup(modelP->mappedV[ix].entityId);
    keptV[kept].attrName = strdup(expanded);
    keptV[kept].nodeId   = (modelP->mappedV[ix].nodeId != NULL) ? strdup(modelP->mappedV[ix].nodeId) : NULL;
    kept++;
  }

  pthread_mutex_lock(&acP->modelMutex);

  AssetMapped* oldV   = acP->mappedV;
  int          oldN   = acP->mapped;

  acP->mappedV = keptV;
  acP->mapped  = (keptV != NULL) ? kept : 0;

  pthread_mutex_unlock(&acP->modelMutex);

  assetChannelMappedFree(oldV, oldN);
}
#endif



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
// referenceResolve - the entity a reference's target is, or is in: a node of this model's, else of another
// AssetChannel's last model (what it became recorded - a new model there may change it)
//
static void referenceResolve(AssetModel* modelP, const char* target, const char** objectP, bool* insideP)
{
  AssetEntity* inP   = NULL;
  CorNode*     nodeP = (modelP->rootP != NULL) ? nodeById(modelP, modelP->rootP, target, NULL, &inP) : NULL;

  *objectP = NULL;
  *insideP = false;

  if (nodeP != NULL)                                  // this model's - in an entity, or in none: a Property
  {
    if (inP != NULL)
    {
      *objectP = inP->id;
      *insideP = (inP->nodeP != nodeP);
    }
    return;
  }

  char* entityId = NULL;
  bool  isEntity = false;

  if (assetChannelNodeLookup(modelP->acP, target, &entityId, &isEntity) == true)
  {
    *objectP = entityId;
    *insideP = (isEntity == false);
  }

  for (int ix = 0; ix < modelP->refs; ix++)
  {
    if (strcmp(modelP->refV[ix].target, target) == 0)
      return;
  }

  if (modelP->refs < modelP->refsMax)
  {
    AssetReference* refP = &modelP->refV[modelP->refs++];

    refP->target   = (char*) target;
    refP->entityId = entityId;
    refP->isEntity = isEntity;
  }
}



// -----------------------------------------------------------------------------
//
// referencesMap - a node's references, resolved, for the attributes of the entity (or the attribute) they are
// in - made at the end of the entity, referenceAttrsAdd
//
static void referencesMap(AssetModel* modelP, CorNode* holderP, CorNode* nodeP)
{
  CorNode* refsP = corTreeLookup(nodeP, "references");

  for (CorNode* refP = ((refsP != NULL) && (refsP->type == CorArray)) ? refsP->value.head : NULL; refP != NULL; refP = refP->next)
  {
    const char* target = str(refP, "target");
    char*       name   = attrName(NULL, str(refP, "type"));

    if ((name == NULL) || (target == NULL) || (modelP->refAttrs >= modelP->refsMax))
      continue;

    AssetRefAttr* rP = &modelP->refAttrV[modelP->refAttrs++];

    rP->holderP = holderP;
    rP->name    = name;
    rP->target  = target;
    referenceResolve(modelP, target, &rP->object, &rP->inside);
  }
}



// -----------------------------------------------------------------------------
//
// referenceAttrsAdd - the references of the entity made, by name: the ones that became an entity a
// Relationship (several a ListRelationship), with a "targetNode" when one is a node in it; none did: the
// first one's target as a Property
//
static void referenceAttrsAdd(AssetModel* modelP)
{
  for (int ix = 0; ix < modelP->refAttrs; ix++)
  {
    AssetRefAttr* rP = &modelP->refAttrV[ix];

    if (rP->name == NULL)
      continue;

    char*       name    = rP->name;
    CorNode*    holderP = rP->holderP;
    const char* first   = rP->target;
    int         objects = 0;
    bool        inside  = false;

    for (int jx = ix; jx < modelP->refAttrs; jx++)
    {
      AssetRefAttr* oP = &modelP->refAttrV[jx];

      if ((oP->name != NULL) && (oP->holderP == holderP) && (strcmp(oP->name, name) == 0) && (oP->object != NULL))
      {
        objects++;
        inside = inside || oP->inside;
      }
    }

    if (objects == 0)                                 // none an entity: the first one's target
    {
      CorNode* propP = corTreeObject(corRest.kallocP, name);

      corTreeChildAdd(propP, corTreeString(corRest.kallocP, "type", "Property"));
      corTreeChildAdd(propP, corTreeString(corRest.kallocP, "value", (char*) first));
      attrAdd(holderP, propP);
    }

    bool     existed  = (corTreeLookup(holderP, name) != NULL);
    CorNode* targetsP = ((inside == true) && (existed == false)) ? corTreeArray(corRest.kallocP, "valueList") : NULL;
    char*    single   = NULL;

    for (int jx = ix; jx < modelP->refAttrs; jx++)
    {
      AssetRefAttr* oP = &modelP->refAttrV[jx];

      if ((oP->name == NULL) || (oP->holderP != holderP) || (strcmp(oP->name, name) != 0))
        continue;

      oP->name = NULL;                                // made

      if (oP->object == NULL)
        continue;

      relationshipAdd(holderP, name, oP->object);

      if (targetsP != NULL)
      {
        corTreeChildAdd(targetsP, corTreeString(corRest.kallocP, NULL, (char*) oP->target));
        single = (char*) oP->target;
      }
    }

    CorNode* attrP = (targetsP != NULL) ? corTreeLookup(holderP, name) : NULL;

    if (attrP != NULL)
    {
      CorNode* subP = corTreeObject(corRest.kallocP, "targetNode");

      if (objects == 1)
      {
        corTreeChildAdd(subP, corTreeString(corRest.kallocP, "type", "Property"));
        corTreeChildAdd(subP, corTreeString(corRest.kallocP, "value", single));
      }
      else
      {
        corTreeChildAdd(subP, corTreeString(corRest.kallocP, "type", "ListProperty"));
        corTreeChildAdd(subP, targetsP);
      }

      corTreeChildAdd(attrP, subP);
    }
  }

  modelP->refAttrs = 0;
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
#if COR_FEATURE_CHANNEL_CHECKS || COR_FEATURE_CHANNEL_WRITE_BACK
      if (entityP == modelP->curEntityP)              // an attribute of the entity, not a sub-attribute
        mappedAdd(modelP, name, str(childP, "id"));
#endif
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

#if COR_FEATURE_CHANNEL_CHECKS || COR_FEATURE_CHANNEL_WRITE_BACK
  modelP->curEntityP  = entityP;
  modelP->curEntityId = entP->id;
#endif

  modelP->refAttrs = 0;
  attrAdd(entityP, semanticIdNode(entP->nodeP));
  childrenMap(modelP, entityP, entP->nodeP, NULL, modelP->mapP->layout);
  referencesMap(modelP, entityP, entP->nodeP);
  referenceAttrsAdd(modelP);

#if COR_FEATURE_CHANNEL_CHECKS || COR_FEATURE_CHANNEL_WRITE_BACK
  //
  // Every other attribute - a container's, a Relationship, the semanticId: no one node it comes from
  //
  for (CorNode* attrP = entityP->value.head; attrP != NULL; attrP = attrP->next)
  {
    if ((strcmp(attrP->name, "id") != 0) && (strcmp(attrP->name, "type") != 0))
      mappedAdd(modelP, attrP->name, NULL);
  }

  modelP->curEntityP = NULL;
#endif

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
// refCount - the references of a tree
//
static int refCount(CorNode* nodeP)
{
  CorNode* refsP = corTreeLookup(nodeP, "references");
  int      n     = 0;

  for (CorNode* refP = ((refsP != NULL) && (refsP->type == CorArray)) ? refsP->value.head : NULL; refP != NULL; refP = refP->next)
    n++;

  CorNode* childrenP = corTreeLookup(nodeP, "children");

  for (CorNode* childP = ((childrenP != NULL) && (childrenP->type == CorArray)) ? childrenP->value.head : NULL; childP != NULL; childP = childP->next)
  {
    if (childP->type == CorObject)
      n += refCount(childP);
  }

  return n;
}



// -----------------------------------------------------------------------------
//
// nodesFind - every node with an id that is an entity or is in one, and that entity: what another
// AssetChannel's reference to it becomes
//
static void nodesFind(AssetModel* modelP, CorNode* nodeP, AssetEntity* inP)
{
  AssetEntity* selfP  = entityOfNode(modelP, nodeP);
  const char*  nodeId = str(nodeP, "id");

  if (selfP != NULL)
    inP = selfP;

  if ((inP != NULL) && (inP->id != NULL) && (nodeId != NULL) && (modelP->nodes < modelP->entitiesMax))
  {
    AssetNode* nP = &modelP->nodeV[modelP->nodes++];

    nP->nodeId   = (char*) nodeId;
    nP->entityId = inP->id;
    nP->isEntity = (selfP != NULL);
  }

  CorNode* childrenP = corTreeLookup(nodeP, "children");

  for (CorNode* childP = ((childrenP != NULL) && (childrenP->type == CorArray)) ? childrenP->value.head : NULL; childP != NULL; childP = childP->next)
  {
    if (childP->type == CorObject)
      nodesFind(modelP, childP, inP);
  }
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
static bool modelFind(AssetModel* modelP, AssetChannel* acP, CorNode* treeP)
{
  char path[1024];
  int  nodes = nodeCount(treeP);
  int  refs  = refCount(treeP);

  path[0]             = 0;
  modelP->acP         = acP;
  modelP->mapP        = acP->mapP;
  modelP->rootP       = treeP;
  modelP->entities    = 0;
  modelP->entitiesMax = nodes;
  modelP->entityV     = (AssetEntity*) corAlloc(&corRest.kalloc, nodes * sizeof(AssetEntity));
  modelP->nodes       = 0;
  modelP->nodeV       = (AssetNode*) corAlloc(&corRest.kalloc, nodes * sizeof(AssetNode));
  modelP->refs        = 0;
  modelP->refsMax     = refs;
  modelP->refV        = (refs > 0) ? (AssetReference*) corAlloc(&corRest.kalloc, refs * sizeof(AssetReference)) : NULL;
  modelP->refAttrs    = 0;
  modelP->refAttrV    = (refs > 0) ? (AssetRefAttr*) corAlloc(&corRest.kalloc, refs * sizeof(AssetRefAttr)) : NULL;
#if COR_FEATURE_CHANNEL_CHECKS || COR_FEATURE_CHANNEL_WRITE_BACK
  modelP->curEntityP  = NULL;
  modelP->curEntityId = NULL;
  modelP->mappedV     = NULL;
  modelP->mapped      = 0;
  modelP->mappedMax   = 0;
#endif

  if ((modelP->entityV == NULL) || (modelP->nodeV == NULL) || ((refs > 0) && ((modelP->refV == NULL) || (modelP->refAttrV == NULL))))
  {
    modelP->refsMax = 0;
    return false;
  }

  entitiesFind(modelP, treeP, path, sizeof(path));
  nodesFind(modelP, treeP, NULL);
  return true;
}



#if COR_FEATURE_ASSET_CHANNEL_SERVICES
// -----------------------------------------------------------------------------
//
// operationsFind - the operations of an entity node: its children's, and those in its containers - not those of
// an entity child (they are that entity's)
//
static void operationsFind(AssetModel* modelP, AssetEntity* entP, CorNode* nodeP, AssetOperation* opV, int* opsP, int opsMax)
{
  CorNode* childrenP = corTreeLookup(nodeP, "children");

  for (CorNode* childP = ((childrenP != NULL) && (childrenP->type == CorArray)) ? childrenP->value.head : NULL; childP != NULL; childP = childP->next)
  {
    if (childP->type != CorObject)
      continue;

    if (isMethod(childP) == true)
    {
      if (*opsP < opsMax)
      {
        opV[*opsP].entityId   = entP->id;
        opV[*opsP].entityType = entP->type;
        opV[*opsP].opP        = childP;
        *opsP += 1;
      }
    }
    else if ((isLeaf(childP) == false) && (entityOfNode(modelP, childP) == NULL))
      operationsFind(modelP, entP, childP, opV, opsP, opsMax);
  }
}



// -----------------------------------------------------------------------------
//
// servicesSync - the model's operations as the AssetChannel's Service Registrations
//
static void servicesSync(AssetChannel* acP, AssetModel* modelP)
{
  AssetOperation* opV = (AssetOperation*) corAlloc(&corRest.kalloc, (modelP->entitiesMax + 1) * sizeof(AssetOperation));   // entitiesMax: the model's nodes
  int             ops = 0;

  if (opV == NULL)
    return;

  for (int ix = 0; ix < modelP->entities; ix++)
  {
    if (modelP->entityV[ix].id != NULL)
      operationsFind(modelP, &modelP->entityV[ix], modelP->entityV[ix].nodeP, opV, &ops, modelP->entitiesMax);
  }

  assetChannelServicesSync(acP, opV, ops);
}
#endif



// -----------------------------------------------------------------------------
//
// entityIdTwice - an id two of the model's entities would have; NULL: none
//
// An AAS id, an OPC UA node id, names one thing; two nodes of a model with one id (an AAS shell and a submodel
// given the same id) would be written as one entity, of both types - the model is wrong, not the mapping.
//
static const char* entityIdTwice(AssetModel* modelP)
{
  for (int ix = 1; ix < modelP->entities; ix++)
  {
    const char* id = modelP->entityV[ix].id;

    for (int jx = 0; (id != NULL) && (jx < ix); jx++)
    {
      if ((modelP->entityV[jx].id != NULL) && (strcmp(modelP->entityV[jx].id, id) == 0))
        return id;
    }
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// modelMap - a model mapped and written: one in ({ "model": <node> }), or the kept one again (remap: not kept
// anew, not counted as a model in, its services as they are). Under writeMutex
//
static int modelMap(AssetChannel* acP, CorNode* treeP, bool remap)
{
  AssetModel model;

  if ((treeP->type != CorObject) || (modelFind(&model, acP, treeP) == false) || (model.entities == 0))
  {
    COR_W("bridge '%s': the model on AssetChannel '%s' makes no entity - dropped", acP->bridgeName, acP->endpoint);
    __atomic_add_fetch(&acP->samplesRefused, 1, __ATOMIC_RELAXED);
    corNgsildFallbackRelease();
    return BRIDGE_BAD_INPUT;
  }

  const char* twice = entityIdTwice(&model);

  if (twice != NULL)
  {
    char reason[512];

    snprintf(reason, sizeof(reason), "its model has two nodes with the id '%s' - one id names one thing: the model is refused", twice);
    COR_W("bridge '%s': AssetChannel '%s': %s", acP->bridgeName, acP->endpoint, reason);
    __atomic_add_fetch(&acP->samplesRefused, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&acP->modelRefused, true, __ATOMIC_RELEASE);
    assetChannelStatusSet(acP->bridgeName, acP->endpoint, ChannelStatusDormant, reason);
    corNgsildFallbackRelease();
    return BRIDGE_BAD_INPUT;
  }

  if (remap == false)
  {
    pthread_mutex_lock(&acP->modelMutex);
    modelKeep(acP, treeP);
    pthread_mutex_unlock(&acP->modelMutex);
  }

  int written = write(acP, &model);

  if (written == 0)
    return BRIDGE_ERR;

  assetChannelNodesKeep(acP, model.nodeV, model.nodes, model.refV, model.refs);   // the entities written: another model's references to them

#if COR_FEATURE_CHANNEL_CHECKS || COR_FEATURE_CHANNEL_WRITE_BACK
  mappedKeep(acP, &model);                            // what a client's write is checked against and written back to - the model's names; values do not change them
#endif

  __atomic_add_fetch(&acP->entitiesWritten, (uint64_t) written, __ATOMIC_RELAXED);

  if (remap == true)
  {
    COR_T(CtBridge, "model on AssetChannel '%s' mapped again (a reference's target changed) -> %d entit%s", acP->endpoint, written, (written == 1) ? "y" : "ies");
    return BRIDGE_OK;
  }

#if COR_FEATURE_ASSET_CHANNEL_SERVICES
  servicesSync(acP, &model);                          // a new model, its operations: values do not change them
#endif

  if (__atomic_exchange_n(&acP->modelRefused, false, __ATOMIC_ACQ_REL) == true)
    assetChannelStatusSet(acP->bridgeName, acP->endpoint, ChannelStatusAvailable, NULL);   // a model taken: available again

  __atomic_add_fetch(&acP->modelsIn, 1, __ATOMIC_RELAXED);
  COR_T(CtBridge, "model on AssetChannel '%s' -> %d entit%s", acP->endpoint, written, (written == 1) ? "y" : "ies");

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// modelRemap - an AssetChannel's kept model mapped and written again: a reference in it now becomes something else
//
static void modelRemap(AssetChannel* acP)
{
  pthread_mutex_lock(&acP->writeMutex);

  pthread_mutex_lock(&acP->modelMutex);
  char* text = (acP->model != NULL) ? corAllocStrdup(&corRest.kalloc, acP->model) : NULL;
  pthread_mutex_unlock(&acP->modelMutex);

  CorNode* treeP = (text != NULL) ? corJsonParse(corRest.corJsonP, text) : NULL;

  if (treeP != NULL)
    modelMap(acP, treeP, true);

  pthread_mutex_unlock(&acP->writeMutex);
}



// -----------------------------------------------------------------------------
//
// modelIn - { "model": <node> }; then the other AssetChannels whose references it changes, mapped again
//
// One AssetChannel's writeMutex at a time: this one's released before another's is taken - two models
// coming in at once, each changing the other's references, wait for no one.
//
static int modelIn(AssetChannel* acP, CorNode* treeP)
{
  pthread_mutex_lock(&acP->writeMutex);
  int r = modelMap(acP, treeP, false);
  pthread_mutex_unlock(&acP->writeMutex);

  if (r != BRIDGE_OK)
    return r;

  AssetChannel** acV = NULL;
  int            n   = assetChannelsToRemap(acP, &acV);

  for (int ix = 0; ix < n; ix++)
    modelRemap(acV[ix]);

  return r;
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

  if ((treeP == NULL) || (modelFind(&model, acP, treeP) == false))
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



#if COR_FEATURE_CHANNEL_WRITE_BACK
// -----------------------------------------------------------------------------
//
// assetChannelNodeValueSet -
//
void assetChannelNodeValueSet(AssetChannel* acP, const char* nodeId, CorNode* valueP)
{
  pthread_mutex_lock(&acP->modelMutex);

  char*      text  = (acP->model != NULL) ? corAllocStrdup(&corRest.kalloc, acP->model) : NULL;
  CorNode*   treeP = (text != NULL) ? corJsonParse(corRest.corJsonP, text) : NULL;
  AssetModel model;

  if ((treeP != NULL) && (modelFind(&model, acP, treeP) == true))
  {
    AssetEntity* inP   = NULL;
    CorNode*     nodeP = nodeById(&model, treeP, nodeId, NULL, &inP);

    if (nodeP != NULL)
    {
      memberSet(nodeP, "value", valueP);
      modelKeep(acP, treeP);
    }
  }

  pthread_mutex_unlock(&acP->modelMutex);
}
#endif



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
  {
    pthread_mutex_lock(&acP->writeMutex);
    r = valuesIn(acP, valuesP);
    pthread_mutex_unlock(&acP->writeMutex);
  }
  else
  {
    COR_W("bridge '%s': the sample on AssetChannel '%s' is neither { \"model\": ... } nor { \"values\": [ ... ] } - dropped", acP->bridgeName, acP->endpoint);
    __atomic_add_fetch(&acP->samplesRefused, 1, __ATOMIC_RELAXED);
    corNgsildFallbackRelease();
    r = BRIDGE_BAD_INPUT;
  }

  return r;
}
