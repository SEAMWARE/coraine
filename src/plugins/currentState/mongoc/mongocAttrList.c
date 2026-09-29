//
// FILE            mongocAttrList.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Discovery § 5.7.8 / § 5.7.9 / § 5.7.10:
// open a cursor over the tenant's entities collection, build per-attr
// aggregation of entity type names, attribute type sets and counts.
//

#include <mongoc/mongoc.h>                              // mongoc_collection_*
#include <string.h>                                     // strcmp

#include "corLog/corLog.h"                              // COR_E
#include "corTree/CorNode.h"                            // CorNode
#include "corTree/corTreeBuilder.h"                     // corTreeArray, corTreeObject, corTreeString, corTreeInteger, corTreeChildAdd
#include "corTree/corTreeLookup.h"                      // corTreeLookup
#include "corRest/CorRestState.h"                         // corRest

#include "corNgsild/ldIsEntityKeyword.h"                  // ldIsEntityKeyword
#include "corNgsild/LdAttrType.h"                        // LdAttrType
#include "corNgsild/ldAttrTypeDetect.h"                  // ldAttrTypeDetect
#include "corNgsild/ldTypes.h"                           // ldAttrTypeToString

#include "db/DbDriver.h"                                // DB_OK, DB_ERR
#include "db/Tenant.h"                                  // Tenant
#include "currentState/mongoc/mongocBsonToTree.h"       // mongocBsonToTree
#include "currentState/mongoc/mongocAttrList.h"         // Own interface



extern mongoc_client_pool_t* poolP;



static CorNode* attrEntryLookup(CorNode* result, const char* attrIri, bool details)
{
  for (CorNode* entry = result->value.head; entry != NULL; entry = entry->next)
  {
    CorNode* iriP = corTreeLookup(entry, "attrIri");
    if (iriP != NULL && iriP->type == CorString && strcmp(iriP->value.s, attrIri) == 0)
      return entry;
  }

  CorNode* entry = corTreeObject(corRest.kallocP, NULL);
  corTreeChildAdd(entry, corTreeString(corRest.kallocP, "attrIri", attrIri));
  if (details)
  {
    corTreeChildAdd(entry, corTreeArray(corRest.kallocP, "typeNames"));
    corTreeChildAdd(entry, corTreeArray(corRest.kallocP, "attrTypes"));
    corTreeChildAdd(entry, corTreeInteger(corRest.kallocP, "attrCount", 0));
  }

  corTreeChildAdd(result, entry);
  return entry;
}



static void stringArrayAddUnique(CorNode* arr, const char* s)
{
  for (CorNode* p = arr->value.head; p != NULL; p = p->next)
    if (p->type == CorString && strcmp(p->value.s, s) == 0)
      return;
  corTreeChildAdd(arr, corTreeString(corRest.kallocP, NULL, s));
}



static CorNode* firstInstance(CorNode* attrP)
{
  if (attrP == NULL || attrP->type != CorObject)
    return NULL;
  for (CorNode* instP = attrP->value.head; instP != NULL; instP = instP->next)
    if (instP->type == CorObject)
      return instP;
  return NULL;
}



static int instanceCount(CorNode* attrP)
{
  int n = 0;
  if (attrP == NULL || attrP->type != CorObject) return 0;
  for (CorNode* instP = attrP->value.head; instP != NULL; instP = instP->next)
    if (instP->type == CorObject) n++;
  return n;
}



static void recordTypeNamesFromEntity(CorNode* typeNamesArr, CorNode* typeP)
{
  if (typeP == NULL) return;

  if (typeP->type == CorString)
  {
    stringArrayAddUnique(typeNamesArr, typeP->value.s);
  }
  else if (typeP->type == CorArray)
  {
    for (CorNode* tN = typeP->value.head; tN != NULL; tN = tN->next)
      if (tN->type == CorString)
        stringArrayAddUnique(typeNamesArr, tN->value.s);
  }
}



// -----------------------------------------------------------------------------
//
// mongocAttrList -
//
int mongocAttrList(Tenant* tenantP, bool details, CorNode** arrayPP)
{
  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  bson_t filter;
  bson_init(&filter);
  mongoc_cursor_t* cursorP = mongoc_collection_find_with_opts(collP, &filter, NULL, NULL);

  CorNode* result = corTreeArray(corRest.kallocP, NULL);
  *arrayPP = result;

  const bson_t* doc;
  while (mongoc_cursor_next(cursorP, &doc))
  {
    CorNode* eP = mongocBsonToTree(&corRest.kalloc, doc);
    if (eP == NULL) continue;

    CorNode* typeP = corTreeLookup(eP, "type");

    for (CorNode* attrP = eP->value.head; attrP != NULL; attrP = attrP->next)
    {
      if (ldIsEntityMember(attrP)) continue;

      CorNode* entry = attrEntryLookup(result, attrP->name, details);
      if (!details) continue;

      recordTypeNamesFromEntity(corTreeLookup(entry, "typeNames"), typeP);

      CorNode* attrTypesArr = corTreeLookup(entry, "attrTypes");
      CorNode* instP       = firstInstance(attrP);
      LdAttrType at        = ldAttrTypeDetect(instP);
      if (at != LdAttrNone)
      {
        const char* atStr = ldAttrTypeToString(at);
        if (atStr != NULL)
          stringArrayAddUnique(attrTypesArr, atStr);
      }

      CorNode* countP = corTreeLookup(entry, "attrCount");
      if (countP != NULL) countP->value.i += instanceCount(attrP);
    }
  }

  bson_error_t error;
  int          rr = DB_OK;
  if (mongoc_cursor_error(cursorP, &error))
  {
    COR_E("mongoc: attrList cursor failed: %s", error.message);
    rr = DB_ERR;
  }

  mongoc_cursor_destroy(cursorP);
  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return rr;
}
