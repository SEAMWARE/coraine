//
// FILE            corDbAttrList.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Discovery § 5.7.8 / § 5.7.9 / § 5.7.10:
// aggregate distinct attribute names across all locally stored entities,
// plus (on details) the entity-type names they appear on, attribute types
// seen and instance counts.
//

#include <string.h>                                     // strcmp

#include "corTree/CorNode.h"                            // CorNode
#include "corTree/corTreeBuilder.h"                     // corTreeArray, corTreeObject, corTreeString, corTreeInteger, corTreeChildAdd
#include "corTree/corTreeLookup.h"                      // corTreeLookup
#include "corRest/CorRestState.h"                         // corRest

#include "corNgsild/ldIsEntityKeyword.h"                 // ldIsEntityKeyword
#include "corNgsild/LdAttrType.h"                        // LdAttrType
#include "corNgsild/ldAttrTypeDetect.h"                  // ldAttrTypeDetect
#include "corNgsild/ldTypes.h"                           // ldAttrTypeToString

#include "db/DbDriver.h"                                // DB_OK
#include "db/Tenant.h"                                  // Tenant
#include "currentState/corDB/corDbStore.h"            // corDbEntities
#include "currentState/corDB/corDbAttrList.h"         // Own interface



// -----------------------------------------------------------------------------
//
// attrEntryLookup - find or create an entry for an attribute IRI
//
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
// instanceCount - number of dsKey-keyed instances in an attr wrapper
//
static int instanceCount(CorNode* attrP)
{
  int n = 0;
  if (attrP == NULL || attrP->type != CorObject) return 0;
  for (CorNode* instP = attrP->value.head; instP != NULL; instP = instP->next)
    if (instP->type == CorObject) n++;
  return n;
}



// -----------------------------------------------------------------------------
//
// recordTypeNamesFromEntity - add each entity-type IRI to entry.typeNames
//
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
// corDbAttrList -
//
int corDbAttrList(Tenant* tenantP, bool details, CorNode** arrayPP)
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

    for (CorNode* attrP = eP->value.head; attrP != NULL; attrP = attrP->next)
    {
      if (ldIsEntityMember(attrP)) continue;

      CorNode* entry = attrEntryLookup(result, attrP->name, details);

      if (!details)
        continue;

      recordTypeNamesFromEntity(corTreeLookup(entry, "typeNames"), typeP);

      // attrTypes — seen across instances of this attr
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

  return DB_OK;
}
