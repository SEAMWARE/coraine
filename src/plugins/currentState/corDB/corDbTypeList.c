//
// FILE            corDbTypeList.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Discovery § 5.7.5 / § 5.7.6 / § 5.7.7:
// aggregate distinct entity types across all locally stored entities,
// plus (on details) their attribute names, attribute type sets and
// per-type entity counts.
//

#include <string.h>                                     // strcmp

#include "corTree/CorNode.h"                            // CorNode
#include "corTree/corTreeBuilder.h"                     // corTreeArray, corTreeObject, corTreeString, corTreeInteger, corTreeChildAdd
#include "corTree/corTreeLookup.h"                      // corTreeLookup
#include "corRest/CorRestState.h"                         // corRest

#include "corNgsild/ldIsEntityKeyword.h"                  // ldIsEntityKeyword
#include "corNgsild/LdAttrType.h"                        // LdAttrType
#include "corNgsild/ldAttrTypeDetect.h"                  // ldAttrTypeDetect
#include "corNgsild/ldTypes.h"                           // ldAttrTypeToString

#include "db/DbDriver.h"                                // DB_OK
#include "db/Tenant.h"                                  // Tenant
#include "currentState/corDB/corDbStore.h"            // corDbEntities
#include "currentState/corDB/corDbTypeList.h"         // Own interface



// -----------------------------------------------------------------------------
//
// typeEntryLookup - find or create an entry for a given type IRI in result
//
static CorNode* typeEntryLookup(CorNode* result, const char* typeIri, bool details)
{
  for (CorNode* entry = result->value.head; entry != NULL; entry = entry->next)
  {
    CorNode* iriP = corTreeLookup(entry, "typeIri");
    if (iriP != NULL && iriP->type == CorString && strcmp(iriP->value.s, typeIri) == 0)
      return entry;
  }

  CorNode* entry = corTreeObject(corRest.kallocP, NULL);
  corTreeChildAdd(entry, corTreeString(corRest.kallocP, "typeIri", typeIri));

  if (details)
  {
    corTreeChildAdd(entry, corTreeArray(corRest.kallocP, "attrs"));
    corTreeChildAdd(entry, corTreeObject(corRest.kallocP, "attrTypes"));
    corTreeChildAdd(entry, corTreeInteger(corRest.kallocP, "entityCount", 0));
  }
  else
  {
    corTreeChildAdd(entry, corTreeArray(corRest.kallocP, "attrs"));
  }

  corTreeChildAdd(result, entry);
  return entry;
}



// -----------------------------------------------------------------------------
//
// stringArrayAddUnique -
//
static void stringArrayAddUnique(CorNode* arr, const char* s)
{
  for (CorNode* p = arr->value.head; p != NULL; p = p->next)
    if (p->type == CorString && strcmp(p->value.s, s) == 0)
      return;
  corTreeChildAdd(arr, corTreeString(corRest.kallocP, NULL, s));
}



// -----------------------------------------------------------------------------
//
// firstInstance - first CorObject child of an attr wrapper (dsKey-keyed)
//
static CorNode* firstInstance(CorNode* attrP)
{
  if (attrP == NULL || attrP->type != CorObject)
    return NULL;
  for (CorNode* instP = attrP->value.head; instP != NULL; instP = instP->next)
    if (instP->type == CorObject)
      return instP;
  return NULL;
}



// -----------------------------------------------------------------------------
//
// recordAttr - add attrName to entry.attrs (+ attrTypes when details)
//
static void recordAttr(CorNode* typeEntry, const char* attrName, CorNode* attrWrapper, bool details)
{
  CorNode* attrs = corTreeLookup(typeEntry, "attrs");
  stringArrayAddUnique(attrs, attrName);

  if (!details)
    return;

  CorNode* attrTypesObj = corTreeLookup(typeEntry, "attrTypes");

  CorNode* instP = firstInstance(attrWrapper);
  LdAttrType at = ldAttrTypeDetect(instP);
  if (at == LdAttrNone)
    return;

  const char* atStr = ldAttrTypeToString(at);
  if (atStr == NULL)
    return;

  CorNode* attrTypeArr = corTreeLookup(attrTypesObj, attrName);
  if (attrTypeArr == NULL)
  {
    attrTypeArr = corTreeArray(corRest.kallocP, attrName);
    corTreeChildAdd(attrTypesObj, attrTypeArr);
  }
  stringArrayAddUnique(attrTypeArr, atStr);
}



// -----------------------------------------------------------------------------
//
// corDbTypeList -
//
int corDbTypeList(Tenant* tenantP, bool details, CorNode** arrayPP)
{
  COR_DB_READ(tenantP);

  CorNode* result = corTreeArray(corRest.kallocP, NULL);
  *arrayPP = result;

  CorNode* entities = corDbEntities(tenantP);
  if (entities == NULL)
    return DB_OK;

  for (CorNode* eP = entities->value.head; eP != NULL; eP = eP->next)
  {
    CorNode* typeP = corTreeLookup(eP, "type");
    if (typeP == NULL)
      continue;

    //
    // Normalize type → iterate strings
    //
    const char* typeV[16];
    int typeN = 0;

    if (typeP->type == CorString)
    {
      typeV[typeN++] = typeP->value.s;
    }
    else if (typeP->type == CorArray)
    {
      for (CorNode* tN = typeP->value.head; tN != NULL && typeN < 16; tN = tN->next)
        if (tN->type == CorString)
          typeV[typeN++] = tN->value.s;
    }

    for (int t = 0; t < typeN; t++)
    {
      CorNode* typeEntry = typeEntryLookup(result, typeV[t], details);

      if (details)
      {
        CorNode* countP = corTreeLookup(typeEntry, "entityCount");
        if (countP != NULL) countP->value.i++;
      }

      for (CorNode* attrP = eP->value.head; attrP != NULL; attrP = attrP->next)
      {
        if (ldIsEntityMember(attrP)) continue;
        recordAttr(typeEntry, attrP->name, attrP, details);
      }
    }
  }

  return DB_OK;
}
