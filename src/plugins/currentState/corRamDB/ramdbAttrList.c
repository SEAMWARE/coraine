//
// FILE            ramdbAttrList.c
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

#include "kjson/KjNode.h"                               // KjNode
#include "kjson/kjBuilder.h"                            // kjArray, kjObject, kjString, kjInteger, kjChildAdd
#include "kjson/kjLookup.h"                             // kjLookup
#include "corRest/CorRestState.h"                         // corRest

#include "corNgsild/ldIsEntityKeyword.h"                 // ldIsEntityKeyword
#include "corNgsild/LdAttrType.h"                        // LdAttrType
#include "corNgsild/ldAttrTypeDetect.h"                  // ldAttrTypeDetect
#include "corNgsild/ldTypes.h"                           // ldAttrTypeToString

#include "db/DbDriver.h"                                // DB_OK
#include "db/Tenant.h"                                  // Tenant
#include "currentState/corRamDB/ramdbStore.h"            // ramdbEntities
#include "currentState/corRamDB/ramdbAttrList.h"         // Own interface



// -----------------------------------------------------------------------------
//
// attrEntryLookup - find or create an entry for an attribute IRI
//
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
    kjChildAdd(entry, kjArray(corRest.kjsonP,  "typeNames"));
    kjChildAdd(entry, kjArray(corRest.kjsonP,  "attrTypes"));
    kjChildAdd(entry, kjInteger(corRest.kjsonP, "attrCount", 0));
  }

  kjChildAdd(result, entry);
  return entry;
}



// -----------------------------------------------------------------------------
//
// stringArrayAddUnique -
//
static void stringArrayAddUnique(KjNode* arr, const char* s)
{
  for (KjNode* p = arr->value.firstChildP; p != NULL; p = p->next)
    if (p->type == KjString && strcmp(p->value.s, s) == 0)
      return;
  kjChildAdd(arr, kjString(corRest.kjsonP, NULL, s));
}



// -----------------------------------------------------------------------------
//
// firstInstance - first KjObject child of an attr wrapper (dsKey-keyed)
//
static KjNode* firstInstance(KjNode* attrP)
{
  if (attrP == NULL || attrP->type != KjObject)
    return NULL;
  for (KjNode* instP = attrP->value.firstChildP; instP != NULL; instP = instP->next)
    if (instP->type == KjObject)
      return instP;
  return NULL;
}



// -----------------------------------------------------------------------------
//
// instanceCount - number of dsKey-keyed instances in an attr wrapper
//
static int instanceCount(KjNode* attrP)
{
  int n = 0;
  if (attrP == NULL || attrP->type != KjObject) return 0;
  for (KjNode* instP = attrP->value.firstChildP; instP != NULL; instP = instP->next)
    if (instP->type == KjObject) n++;
  return n;
}



// -----------------------------------------------------------------------------
//
// recordTypeNamesFromEntity - add each entity-type IRI to entry.typeNames
//
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
// ramdbAttrList -
//
int ramdbAttrList(Tenant* tenantP, bool details, KjNode** arrayPP)
{
  KjNode* result = kjArray(corRest.kjsonP, NULL);
  *arrayPP = result;

  KjNode* entities = ramdbEntities(tenantP);
  if (entities == NULL)
    return DB_OK;

  for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
  {
    KjNode* typeP = kjLookup(eP, "type");

    for (KjNode* attrP = eP->value.firstChildP; attrP != NULL; attrP = attrP->next)
    {
      if (ldIsEntityKeyword(attrP->name)) continue;

      KjNode* entry = attrEntryLookup(result, attrP->name, details);

      if (!details)
        continue;

      recordTypeNamesFromEntity(kjLookup(entry, "typeNames"), typeP);

      // attrTypes — seen across instances of this attr
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

  return DB_OK;
}
