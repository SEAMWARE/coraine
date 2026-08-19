//
// FILE            mongocTypeList.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Discovery § 5.7.5 / § 5.7.6 / § 5.7.7:
// open a cursor over the tenant's entities collection, build per-type
// aggregation of attribute names, attribute types and entity counts.
//
// Could be done server-side with $group, but building in memory keeps
// the aggregation rules (type union handling, attribute-type detection
// via ldAttrTypeDetect) identical to the ramdb plugin.
//

#include <mongoc/mongoc.h>                              // mongoc_collection_*
#include <string.h>                                     // strcmp

#include "ktrace/kTrace.h"                              // KT_E
#include "kjson/KjNode.h"                               // KjNode
#include "kjson/kjBuilder.h"                            // kjArray, kjObject, kjString, kjInteger, kjChildAdd
#include "kjson/kjLookup.h"                             // kjLookup
#include "corRest/CorRestState.h"                         // corRest

#include "corNgsild/ldIsEntityKeyword.h"                 // ldIsEntityKeyword
#include "corNgsild/LdAttrType.h"                        // LdAttrType
#include "corNgsild/ldAttrTypeDetect.h"                  // ldAttrTypeDetect
#include "corNgsild/ldTypes.h"                           // ldAttrTypeToString

#include "db/DbDriver.h"                                // DB_OK, DB_ERR
#include "db/Tenant.h"                                  // Tenant
#include "currentState/mongoc/mongocBsonToKjTree.h"     // mongocBsonToKjTree
#include "currentState/mongoc/mongocTypeList.h"         // Own interface



extern mongoc_client_pool_t* poolP;



// -----------------------------------------------------------------------------
//
// typeEntryLookup -
//
static KjNode* typeEntryLookup(KjNode* result, const char* typeIri, bool details)
{
  for (KjNode* entry = result->value.firstChildP; entry != NULL; entry = entry->next)
  {
    KjNode* iriP = kjLookup(entry, "typeIri");
    if (iriP != NULL && iriP->type == KjString && strcmp(iriP->value.s, typeIri) == 0)
      return entry;
  }

  KjNode* entry = kjObject(corRest.kjsonP, NULL);
  kjChildAdd(entry, kjString(corRest.kjsonP, "typeIri", typeIri));
  kjChildAdd(entry, kjArray(corRest.kjsonP, "attrs"));
  if (details)
  {
    kjChildAdd(entry, kjObject(corRest.kjsonP,  "attrTypes"));
    kjChildAdd(entry, kjInteger(corRest.kjsonP, "entityCount", 0));
  }

  kjChildAdd(result, entry);
  return entry;
}



static void stringArrayAddUnique(KjNode* arr, const char* s)
{
  for (KjNode* p = arr->value.firstChildP; p != NULL; p = p->next)
    if (p->type == KjString && strcmp(p->value.s, s) == 0)
      return;
  kjChildAdd(arr, kjString(corRest.kjsonP, NULL, s));
}



static KjNode* firstInstance(KjNode* attrP)
{
  if (attrP == NULL || attrP->type != KjObject)
    return NULL;
  for (KjNode* instP = attrP->value.firstChildP; instP != NULL; instP = instP->next)
    if (instP->type == KjObject)
      return instP;
  return NULL;
}



static void recordAttr(KjNode* typeEntry, const char* attrName, KjNode* attrWrapper, bool details)
{
  KjNode* attrs = kjLookup(typeEntry, "attrs");
  stringArrayAddUnique(attrs, attrName);

  if (!details)
    return;

  KjNode* attrTypesObj = kjLookup(typeEntry, "attrTypes");

  KjNode* instP = firstInstance(attrWrapper);
  LdAttrType at = ldAttrTypeDetect(instP);
  if (at == LdAttrNone)
    return;

  const char* atStr = ldAttrTypeToString(at);
  if (atStr == NULL)
    return;

  KjNode* attrTypeArr = kjLookup(attrTypesObj, attrName);
  if (attrTypeArr == NULL)
  {
    attrTypeArr = kjArray(corRest.kjsonP, attrName);
    kjChildAdd(attrTypesObj, attrTypeArr);
  }
  stringArrayAddUnique(attrTypeArr, atStr);
}



// -----------------------------------------------------------------------------
//
// mongocTypeList -
//
int mongocTypeList(Tenant* tenantP, bool details, KjNode** arrayPP)
{
  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  bson_t filter;
  bson_init(&filter);
  mongoc_cursor_t* cursorP = mongoc_collection_find_with_opts(collP, &filter, NULL, NULL);

  KjNode* result = kjArray(corRest.kjsonP, NULL);
  *arrayPP = result;

  const bson_t* doc;
  while (mongoc_cursor_next(cursorP, &doc))
  {
    KjNode* eP = mongocBsonToKjTree(&corRest.kalloc, doc);
    if (eP == NULL) continue;

    KjNode* typeP = kjLookup(eP, "type");
    if (typeP == NULL) continue;

    const char* typeV[16];
    int typeN = 0;
    if (typeP->type == KjString)
      typeV[typeN++] = typeP->value.s;
    else if (typeP->type == KjArray)
      for (KjNode* tN = typeP->value.firstChildP; tN != NULL && typeN < 16; tN = tN->next)
        if (tN->type == KjString)
          typeV[typeN++] = tN->value.s;

    for (int t = 0; t < typeN; t++)
    {
      KjNode* entry = typeEntryLookup(result, typeV[t], details);

      if (details)
      {
        KjNode* countP = kjLookup(entry, "entityCount");
        if (countP != NULL) countP->value.i++;
      }

      for (KjNode* attrP = eP->value.firstChildP; attrP != NULL; attrP = attrP->next)
      {
        if (ldIsEntityKeyword(attrP->name)) continue;
        recordAttr(entry, attrP->name, attrP, details);
      }
    }
  }

  bson_error_t error;
  int          rr = DB_OK;
  if (mongoc_cursor_error(cursorP, &error))
  {
    KT_E("mongoc: typeList cursor failed: %s", error.message);
    rr = DB_ERR;
  }

  mongoc_cursor_destroy(cursorP);
  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return rr;
}
