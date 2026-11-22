//
// FILE            assetChannelServices.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// An AssetChannel's Operations as Service Registrations - see assetChannelServices.h and doc/asset-channels.md § 4.
//
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // uint32_t
#include <stdio.h>                                    // snprintf
#include <string.h>                                   // strcmp, strncmp, strlen

#include "corBase/corCrc32c.h"                        // corCrc32c
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeArray, corTreeString, corTreeInteger, corTreeBoolean, corTreeChildAdd, corTreeChildRemove
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corJson/corJsonRender.h"                    // corJsonFastRender
#include "corJson/corJsonRenderSize.h"                // corJsonFastRenderSize
#include "corLog/corLog.h"                            // COR_W, COR_T

#include "corRest/corRest.h"                          // corRest
#include "corJsonld/corLdExpandTree.h"                // corLdExpandTree
#include "corNgsild/CorNgsild.h"                      // ldDefaultContext
#include "corNgsild/LdVocab.h"                        // LD_VOCAB_CREATED_AT, LD_VOCAB_MODIFIED_AT
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampCreate, ldSysTimestampModify

#include "db/DbDriver.h"                              // db, DB_OK, DB_NOT_FOUND
#include "serviceExecution/seRegistrationMatch.h"     // seRegistrationNameConflict
#include "bridge/AssetChannel.h"                      // AssetChannel
#include "bridge/assetChannelCache.h"                 // assetChannelFirst
#include "bridge/assetChannelServices.h"              // Own interface
#include "coraineTraceLevels.h"                       // CtService



// -----------------------------------------------------------------------------
//
// ID_PREFIX - what every registration an AssetChannel makes starts with: the AssetChannel's 16 hex digits follow
//
#define ID_PREFIX      "urn:ngsi-ld:ServiceRegistration:assetChannel:"
#define ID_PREFIX_LEN  (sizeof(ID_PREFIX) - 1)



// -----------------------------------------------------------------------------
//
// str - a string member of a node; NULL when absent or not a string
//
static const char* str(CorNode* nodeP, const char* name)
{
  CorNode* memberP = (nodeP != NULL) ? corTreeLookup(nodeP, name) : NULL;

  return ((memberP != NULL) && (memberP->type == CorString)) ? memberP->value.s : NULL;
}



// -----------------------------------------------------------------------------
//
// hash16 - 16 hex digits of a, or of a and b (two CRC-32Cs, different seeds); out holds 17
//
static void hash16(const char* a, const char* b, char* out)
{
  uint32_t h1 = corCrc32c(0,          a, strlen(a));
  uint32_t h2 = corCrc32c(0x9E3779B9, a, strlen(a));

  if (b != NULL)
  {
    h1 = corCrc32c(corCrc32c(h1, "\t", 1), b, strlen(b));   // a tab is in no bridge name nor endpoint (assetChannelCreate)
    h2 = corCrc32c(corCrc32c(h2, "\t", 1), b, strlen(b));
  }

  snprintf(out, 17, "%08x%08x", h1, h2);
}



// -----------------------------------------------------------------------------
//
// XsType - an XML Schema datatype (an AAS variable's valueType) as a JSON Schema
//
typedef struct XsType
{
  const char*  name;                                  // without "xs:"
  const char*  type;                                  // the JSON Schema type
  bool         hasMin;
  long long    min;
  bool         hasMax;
  long long    max;
} XsType;

static const XsType xsTypeV[] =
{
  { "double",             "number",  false, 0,           false, 0          },
  { "float",              "number",  false, 0,           false, 0          },
  { "decimal",            "number",  false, 0,           false, 0          },
  { "integer",            "integer", false, 0,           false, 0          },
  { "long",               "integer", false, 0,           false, 0          },
  { "int",                "integer", true,  -2147483648, true,  2147483647 },
  { "short",              "integer", true,  -32768,      true,  32767      },
  { "byte",               "integer", true,  -128,        true,  127        },
  { "unsignedLong",       "integer", true,  0,           false, 0          },
  { "unsignedInt",        "integer", true,  0,           true,  4294967295 },
  { "unsignedShort",      "integer", true,  0,           true,  65535      },
  { "unsignedByte",       "integer", true,  0,           true,  255        },
  { "nonNegativeInteger", "integer", true,  0,           false, 0          },
  { "positiveInteger",    "integer", true,  1,           false, 0          },
  { "nonPositiveInteger", "integer", false, 0,           true,  0          },
  { "negativeInteger",    "integer", false, 0,           true,  -1         },
  { "boolean",            "boolean", false, 0,           false, 0          },
  { NULL,                 NULL,      false, 0,           false, 0          }
};



// -----------------------------------------------------------------------------
//
// variableSchema - one variable's JSON Schema: { "type", "minimum", "maximum" } - every other valueType
// (string, dateTime, anyURI, ...) a string, no valueType any value. An array variable ("array": true, an
// OPC UA argument of a ValueRank of dimensions): { "type": "array", "items": that }
//
static CorNode* variableSchema(const char* name, const char* valueType, bool array)
{
  CorNode*      schemaP = corTreeObject(corRest.kallocP, array ? "items" : name);
  const XsType* xsP     = NULL;

  if ((valueType != NULL) && (strncmp(valueType, "xs:", 3) == 0))
    valueType = &valueType[3];

  for (int ix = 0; (valueType != NULL) && (xsTypeV[ix].name != NULL); ix++)
  {
    if (strcmp(xsTypeV[ix].name, valueType) == 0)
    {
      xsP = &xsTypeV[ix];
      break;
    }
  }

  if (valueType != NULL)
    corTreeChildAdd(schemaP, corTreeString(corRest.kallocP, "type", (xsP != NULL) ? xsP->type : "string"));

  if ((xsP != NULL) && (xsP->hasMin == true))
    corTreeChildAdd(schemaP, corTreeInteger(corRest.kallocP, "minimum", xsP->min));
  if ((xsP != NULL) && (xsP->hasMax == true))
    corTreeChildAdd(schemaP, corTreeInteger(corRest.kallocP, "maximum", xsP->max));

  if (array == false)
    return schemaP;

  CorNode* arrayP = corTreeObject(corRest.kallocP, name);

  corTreeChildAdd(arrayP, corTreeString(corRest.kallocP, "type", "array"));
  corTreeChildAdd(arrayP, schemaP);

  return arrayP;
}



// -----------------------------------------------------------------------------
//
// variablesSchema - an operation's input or output variables as a JSON Schema: an object of exactly them -
// every variable required, nothing else (what the bridge invokes the operation with, and what it returns)
//
static CorNode* variablesSchema(const char* name, CorNode* varsP)
{
  CorNode* schemaP   = corTreeObject(corRest.kallocP, name);
  CorNode* propsP    = corTreeObject(corRest.kallocP, "properties");
  CorNode* requiredP = corTreeArray(corRest.kallocP, "required");

  for (CorNode* varP = ((varsP != NULL) && (varsP->type == CorArray)) ? varsP->value.head : NULL; varP != NULL; varP = varP->next)
  {
    const char* varName = str(varP, "name");

    if ((varName == NULL) || (corTreeLookup(propsP, varName) != NULL))
      continue;

    CorNode* arrayP = corTreeLookup(varP, "array");
    bool     array  = (arrayP != NULL) && (arrayP->type == CorBoolean) && (arrayP->value.b == true);

    corTreeChildAdd(propsP,    variableSchema(varName, str(varP, "valueType"), array));
    corTreeChildAdd(requiredP, corTreeString(corRest.kallocP, NULL, varName));
  }

  corTreeChildAdd(schemaP, corTreeString(corRest.kallocP, "type", "object"));
  corTreeChildAdd(schemaP, propsP);

  if (requiredP->value.head != NULL)
    corTreeChildAdd(schemaP, requiredP);

  corTreeChildAdd(schemaP, corTreeBoolean(corRest.kallocP, "additionalProperties", false));

  return schemaP;
}



// -----------------------------------------------------------------------------
//
// registrationMake - an operation's Service Registration, expanded as a POSTed one is
//
static CorNode* registrationMake(AssetOperation* opP, const char* id, const char* name, const char* endpoint)
{
  CorNode* regP      = corTreeObject(corRest.kallocP, NULL);
  CorNode* entitiesP = corTreeArray(corRest.kallocP, "entities");
  CorNode* entityP   = corTreeObject(corRest.kallocP, NULL);
  CorNode* siP       = corTreeObject(corRest.kallocP, "serviceInformation");

  corTreeChildAdd(entityP, corTreeString(corRest.kallocP, "id",   opP->entityId));
  corTreeChildAdd(entityP, corTreeString(corRest.kallocP, "type", opP->entityType));
  corTreeChildAdd(entitiesP, entityP);

  corTreeChildAdd(siP, corTreeString(corRest.kallocP, "serviceName", name));
  corTreeChildAdd(siP, variablesSchema("inputSchema",  corTreeLookup(opP->opP, "input")));
  corTreeChildAdd(siP, variablesSchema("outputSchema", corTreeLookup(opP->opP, "output")));

  corTreeChildAdd(regP, corTreeString(corRest.kallocP, "id",   id));
  corTreeChildAdd(regP, corTreeString(corRest.kallocP, "type", "ServiceRegistration"));
  corTreeChildAdd(regP, entitiesP);
  corTreeChildAdd(regP, siP);
  corTreeChildAdd(regP, corTreeString(corRest.kallocP, "endpoint", endpoint));

  corLdExpandTree(regP, ldDefaultContext(&corRest.kalloc), &corRest.kalloc);   // as the entities are (assetChannelSampleIn.c)

  return regP;
}



// -----------------------------------------------------------------------------
//
// rendered - a registration as JSON text, without its timestamps (in the request's arena)
//
static char* rendered(CorNode* regP)
{
  CorNode* cloneP = corTreeClone(corRest.kallocP, regP);

  if (cloneP == NULL)
    return NULL;

  CorNode* tsP;

  if ((tsP = corTreeLookup(cloneP, LD_VOCAB_CREATED_AT))  != NULL) corTreeChildRemove(cloneP, tsP);
  if ((tsP = corTreeLookup(cloneP, LD_VOCAB_MODIFIED_AT)) != NULL) corTreeChildRemove(cloneP, tsP);

  int   size = corJsonFastRenderSize(cloneP) + 1;
  char* buf  = (char*) corAlloc(&corRest.kalloc, size);

  if (buf != NULL)
    corJsonFastRender(cloneP, buf);

  return buf;
}



// -----------------------------------------------------------------------------
//
// registrationStore - created, replaced (createdAt kept) or, when it did not change, left alone
//
static void registrationStore(AssetChannel* acP, const char* id, CorNode* regP)
{
  CorNode* storedP = NULL;
  int      r       = db.docRetrieve(acP->tenantP, "serviceRegistrations", id, &storedP);

  if ((r == DB_OK) && (storedP != NULL))
  {
    char* was = rendered(storedP);
    char* is  = rendered(regP);

    if ((was != NULL) && (is != NULL) && (strcmp(was, is) == 0))
      return;

    CorNode* createdAtP = corTreeLookup(storedP, LD_VOCAB_CREATED_AT);

    if (createdAtP != NULL)
      corTreeChildAdd(regP, corTreeClone(corRest.kallocP, createdAtP));

    ldSysTimestampModify(regP);

    if (db.docReplace(acP->tenantP, "serviceRegistrations", id, regP) != DB_OK)
      COR_W("bridge '%s': AssetChannel '%s': Service Registration '%s' could not be replaced", acP->bridgeName, acP->endpoint, id);
    else
      COR_T(CtService, "AssetChannel '%s': Service Registration '%s' replaced", acP->endpoint, id);

    return;
  }

  ldSysTimestampCreate(regP);

  if (db.docCreate(acP->tenantP, "serviceRegistrations", id, regP) != DB_OK)
    COR_W("bridge '%s': AssetChannel '%s': Service Registration '%s' could not be created", acP->bridgeName, acP->endpoint, id);
  else
    COR_T(CtService, "AssetChannel '%s': Service Registration '%s' created", acP->endpoint, id);
}



// -----------------------------------------------------------------------------
//
// channelConfigured - is there an AssetChannel whose 16 hex digits these are?
//
static bool channelConfigured(const char* hex)
{
  for (AssetChannel* acP = assetChannelFirst(); acP != NULL; acP = acP->next)
  {
    char h[17];

    hash16(acP->bridgeName, acP->endpoint, h);

    if (strncmp(h, hex, 16) == 0)
      return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// stalesDelete - this AssetChannel's registrations that are not in keptV, and those of AssetChannels the
// broker no longer has
//
static void stalesDelete(AssetChannel* acP, const char* channelHex, char** keptV, int kept)
{
  CorNode* regsP = NULL;

  if ((db.docQuery(acP->tenantP, "serviceRegistrations", &regsP) != DB_OK) || (regsP == NULL))
    return;

  for (CorNode* regP = regsP->value.head; regP != NULL; regP = regP->next)
  {
    const char* id = str(regP, "id");

    if ((id == NULL) || (strncmp(id, ID_PREFIX, ID_PREFIX_LEN) != 0) || (strlen(id) < ID_PREFIX_LEN + 17))
      continue;

    const char* hex   = &id[ID_PREFIX_LEN];
    bool        stale = false;

    if (strncmp(hex, channelHex, 16) == 0)
    {
      stale = true;

      for (int ix = 0; ix < kept; ix++)
      {
        if (strcmp(keptV[ix], id) == 0)
        {
          stale = false;
          break;
        }
      }
    }
    else
      stale = (channelConfigured(hex) == false);

    if (stale == false)
      continue;

    if (db.docDelete(acP->tenantP, "serviceRegistrations", id) != DB_OK)
      COR_W("bridge '%s': AssetChannel '%s': Service Registration '%s' could not be deleted", acP->bridgeName, acP->endpoint, id);
    else
      COR_T(CtService, "AssetChannel '%s': Service Registration '%s' deleted - no such operation now", acP->endpoint, id);
  }
}



// -----------------------------------------------------------------------------
//
// assetChannelServicesSync -
//
void assetChannelServicesSync(AssetChannel* acP, AssetOperation* opV, int ops)
{
  if ((db.docCreate == NULL) || (db.docRetrieve == NULL) || (db.docQuery == NULL) || (db.docReplace == NULL) || (db.docDelete == NULL))
    return;                                           // the database plugin keeps no Service Registrations

  char   channelHex[17];
  char** keptV = (char**) corAlloc(&corRest.kalloc, (ops + 1) * sizeof(char*));
  int    kept  = 0;

  if (keptV == NULL)
    return;

  hash16(acP->bridgeName, acP->endpoint, channelHex);

  for (int ix = 0; ix < ops; ix++)
  {
    AssetOperation* opP      = &opV[ix];
    const char*     name     = str(opP->opP, "name");
    const char*     endpoint = str(opP->opP, "endpoint");

    if ((name == NULL) || (endpoint == NULL))
    {
      COR_T(CtService, "AssetChannel '%s': an operation of '%s' with no %s - not registered", acP->endpoint, opP->entityId, (name == NULL) ? "name" : "endpoint");
      continue;
    }

    char  entityHex[17];
    int   idSize = ID_PREFIX_LEN + 34 + strlen(name) + 1;
    char* id     = (char*) corAlloc(&corRest.kalloc, idSize);

    if (id == NULL)
      return;

    hash16(opP->entityId, NULL, entityHex);
    snprintf(id, idSize, "%s%s:%s:%s", ID_PREFIX, channelHex, entityHex, name);

    bool twice = false;

    for (int kx = 0; kx < kept; kx++)
    {
      if (strcmp(keptV[kx], id) == 0)
      {
        twice = true;
        break;
      }
    }

    if (twice == true)
    {
      COR_W("bridge '%s': AssetChannel '%s': entity '%s' has two operations named '%s' - the second is not registered", acP->bridgeName, acP->endpoint, opP->entityId, name);
      continue;
    }

    CorNode*    regP    = registrationMake(opP, id, name, endpoint);
    const char* otherId = seRegistrationNameConflict(regP);

    if (otherId != NULL)
    {
      COR_W("bridge '%s': AssetChannel '%s': Service Registration '%s' already offers '%s' on entity '%s' - the operation is not registered",
            acP->bridgeName, acP->endpoint, otherId, name, opP->entityId);
      continue;
    }

    registrationStore(acP, id, regP);
    keptV[kept++] = id;
  }

  stalesDelete(acP, channelHex, keptV, kept);
}
