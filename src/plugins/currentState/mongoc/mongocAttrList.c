//
// FILE            mongocAttrList.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Discovery § 5.7.8 / § 5.7.9 / § 5.7.10:
// open a cursor over the tenant's entities collection, build per-attr
// aggregation of entity type names, attribute type sets and counts.
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
#include "currentState/mongoc/mongocAttrList.h"         // Own interface



extern mongoc_client_pool_t* poolP;



static KjNode* attrEntryLookup(KjNode* result, const char* attrIri, bool details)
{
  for (KjNode* entry = result->value.firstChildP; entry != NULL; entry = entry->next)
  {
    KjNode* iriP = kjLookup(entry, "attrIri");
    if (iriP != NULL && iriP->type == KjString && strcmp(iriP->value.s, attrIri) == 0)
      return entry;
  }

  KjNode* entry = kjObject(corRest.kjsonP, NULL);
  kjChildAdd(entry, kjString(corRest.kjsonP, "attrIri", attrIri));
  if (details)
  {
    kjChildAdd(entry, kjArray(corRest.kjsonP,   "typeNames"));
    kjChildAdd(entry, kjArray(corRest.kjsonP,   "attrTypes"));
    kjChildAdd(entry, kjInteger(corRest.kjsonP, "attrCount", 0));
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



static int instanceCount(KjNode* attrP)
{
  int n = 0;
  if (attrP == NULL || attrP->type != KjObject) return 0;
  for (KjNode* instP = attrP->value.firstChildP; instP != NULL; instP = instP->next)
    if (instP->type == KjObject) n++;
  return n;
}



static void recordTypeNamesFromEntity(KjNode* typeNamesArr, KjNode* typeP)
{
  if (typeP == NULL) return;

  if (typeP->type == KjString)
  {
    stringArrayAddUnique(typeNamesArr, typeP->value.s);
  }
  else if (typeP->type == KjArray)
  {
    for (KjNode* tN = typeP->value.firstChildP; tN != NULL; tN = tN->next)
      if (tN->type == KjString)
        stringArrayAddUnique(typeNamesArr, tN->value.s);
  }
}



// -----------------------------------------------------------------------------
//
// mongocAttrList -
//
int mongocAttrList(Tenant* tenantP, bool details, KjNode** arrayPP)
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

    for (KjNode* attrP = eP->value.firstChildP; attrP != NULL; attrP = attrP->next)
    {
      if (ldIsEntityKeyword(attrP->name)) continue;

      KjNode* entry = attrEntryLookup(result, attrP->name, details);
      if (!details) continue;

      recordTypeNamesFromEntity(kjLookup(entry, "typeNames"), typeP);

      KjNode* attrTypesArr = kjLookup(entry, "attrTypes");
      KjNode* instP        = firstInstance(attrP);
      LdAttrType at        = ldAttrTypeDetect(instP);
      if (at != LdAttrNone)
      {
        const char* atStr = ldAttrTypeToString(at);
        if (atStr != NULL)
          stringArrayAddUnique(attrTypesArr, atStr);
      }

      KjNode* countP = kjLookup(entry, "attrCount");
      if (countP != NULL) countP->value.i += instanceCount(attrP);
    }
  }

  bson_error_t error;
  int          rr = DB_OK;
  if (mongoc_cursor_error(cursorP, &error))
  {
    KT_E("mongoc: attrList cursor failed: %s", error.message);
    rr = DB_ERR;
  }

  mongoc_cursor_destroy(cursorP);
  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return rr;
}
