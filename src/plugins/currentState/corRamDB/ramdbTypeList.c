//
// FILE            ramdbTypeList.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Discovery § 5.7.5 / § 5.7.6 / § 5.7.7:
// aggregate distinct entity types across all locally stored entities,
// plus (on details) their attribute names, attribute type sets and
// per-type entity counts.
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
#include "currentState/corRamDB/ramdbTypeList.h"         // Own interface



// -----------------------------------------------------------------------------
//
// typeEntryLookup - find or create an entry for a given type IRI in result
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

  if (details)
  {
    kjChildAdd(entry, kjArray(corRest.kjsonP,  "attrs"));
    kjChildAdd(entry, kjObject(corRest.kjsonP, "attrTypes"));
    kjChildAdd(entry, kjInteger(corRest.kjsonP, "entityCount", 0));
  }
  else
  {
    kjChildAdd(entry, kjArray(corRest.kjsonP, "attrs"));
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
// recordAttr - add attrName to entry.attrs (+ attrTypes when details)
//
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
// ramdbTypeList -
//
int ramdbTypeList(Tenant* tenantP, bool details, KjNode** arrayPP)
{
  KjNode* result = kjArray(corRest.kjsonP, NULL);
  *arrayPP = result;

  KjNode* entities = ramdbEntities(tenantP);
  if (entities == NULL)
    return DB_OK;

  for (KjNode* eP = entities->value.firstChildP; eP != NULL; eP = eP->next)
  {
    KjNode* typeP = kjLookup(eP, "type");
    if (typeP == NULL)
      continue;

    //
    // Normalize type → iterate strings
    //
    const char* typeV[16];
    int typeN = 0;

    if (typeP->type == KjString)
    {
      typeV[typeN++] = typeP->value.s;
    }
    else if (typeP->type == KjArray)
    {
      for (KjNode* tN = typeP->value.firstChildP; tN != NULL && typeN < 16; tN = tN->next)
        if (tN->type == KjString)
          typeV[typeN++] = tN->value.s;
    }

    for (int t = 0; t < typeN; t++)
    {
      KjNode* typeEntry = typeEntryLookup(result, typeV[t], details);

      if (details)
      {
        KjNode* countP = kjLookup(typeEntry, "entityCount");
        if (countP != NULL) countP->value.i++;
      }

      for (KjNode* attrP = eP->value.firstChildP; attrP != NULL; attrP = attrP->next)
      {
        if (ldIsEntityKeyword(attrP->name)) continue;
        recordAttr(typeEntry, attrP->name, attrP, details);
      }
    }
  }

  return DB_OK;
}
