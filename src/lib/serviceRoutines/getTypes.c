//
// FILE            getTypes.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/types — Retrieve Available Entity Types (§ 5.7.5) /
// Retrieve Details of Available Entity Types (§ 5.7.6) when ?details=true.
//
// Mode 1 (?local=true): only local entities are considered. Modes 2/3
// (CSR metadata, dispatch) are not yet wired — the default request is
// currently treated as mode 1.
//

#include <stddef.h>                                   // NULL
#include <string.h>                                   // strcmp

#include "swRest/SwRestState.h"                       // swRest
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjArray, kjObject, kjString, kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjClone.h"                            // kjClone

#include "swJsonld/swldCompact.h"                     // swldCompact
#include "swJsonld/swldInit.h"                        // swldCoreContext

#include "swNgsild/swNgsild.h"                        // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdVocab.h"                         // LD_VOCAB_*
#include "swNgsild/LdRegCache.h"                      // LdRegCache
#include "swNgsild/ldDiscovery.h"                     // ldDiscoveryRegAugmentTypes
#include "swNgsild/ldDiscoveryForward.h"              // ldDiscoveryForwardTypes, ldDiscoveryShouldForward
#include "swNgsild/ldCsourceAlias.h"                  // ldCsourceAliasForTenant

#include "db/DbDriver.h"                              // db, DB_OK
#include "db/Tenant.h"                                // Tenant

#include "serviceRoutines/getTypes.h"                 // Own interface



// -----------------------------------------------------------------------------
//
// shortOrSelf -
//
static const char* shortOrSelf(SwldContext* ctxP, const char* iri)
{
  const char* compact = swldCompact(ctxP, iri);
  return (compact != NULL) ? compact : iri;
}



// -----------------------------------------------------------------------------
//
// getTypes -
//
bool getTypes(void)
{
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  if (db.typeList == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented",
            "type discovery not supported by this DB plugin");
    return true;
  }

  bool details = swNgsild.details;

  KjNode* aggregated = NULL;
  int r = db.typeList(tenantP, details, &aggregated);
  if (r != DB_OK || aggregated == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "type discovery failed");
    return true;
  }

  //
  // Mode 2 (?noForward=true) or default (mode 3): augment with
  // CSR-declared types/attrs. Mode 1 (?local=true) stops at local data.
  //
  if (!swNgsild.local && tenantP->regCacheP != NULL)
    ldDiscoveryRegAugmentTypes(aggregated, (LdRegCache*) tenantP->regCacheP, details);

  //
  // Mode 3 only (default — !local && !noForward): forward the query to
  // every CSR supporting retrieveEntityType(s) and merge the results.
  //
  if (!swNgsild.local && !swNgsild.noForward &&
      tenantP->regCacheP != NULL &&
      ldDiscoveryShouldForward())
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);
    ldDiscoveryForwardTypes(aggregated, (LdRegCache*) tenantP->regCacheP, details, ownAlias);
  }

  SwldContext* ctxP = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();

  if (!details)
  {
    //
    // EntityTypeList (§ 5.2.24): { id, type:"EntityTypeList", typeList:[short names] }
    //
    KjNode* body = kjObject(swRest.kjsonP, NULL);
    kjChildAdd(body, kjString(swRest.kjsonP, "id",   "urn:ngsi-ld:EntityTypeList:local"));
    kjChildAdd(body, kjString(swRest.kjsonP, "type", "EntityTypeList"));

    KjNode* typeList = kjArray(swRest.kjsonP, "typeList");
    for (KjNode* entry = aggregated->value.firstChildP; entry != NULL; entry = entry->next)
    {
      KjNode* iriP = kjLookup(entry, "typeIri");
      if (iriP == NULL || iriP->type != KjString) continue;
      kjChildAdd(typeList, kjString(swRest.kjsonP, NULL, shortOrSelf(ctxP, iriP->value.s)));
    }
    kjChildAdd(body, typeList);

    swRest.out.responseTree   = body;
    swRest.out.httpStatusCode = 200;
    return true;
  }

  //
  // Details: EntityType[] (§ 5.2.25) — array of
  //   { id: <IRI>, type:"EntityType", typeName:<short>, attributeNames:[<short>] }
  //
  KjNode* body = kjArray(swRest.kjsonP, NULL);

  for (KjNode* entry = aggregated->value.firstChildP; entry != NULL; entry = entry->next)
  {
    KjNode* iriP = kjLookup(entry, "typeIri");
    if (iriP == NULL || iriP->type != KjString) continue;

    KjNode* et = kjObject(swRest.kjsonP, NULL);
    kjChildAdd(et, kjString(swRest.kjsonP, "id",       iriP->value.s));
    kjChildAdd(et, kjString(swRest.kjsonP, "type",     "EntityType"));
    kjChildAdd(et, kjString(swRest.kjsonP, "typeName", shortOrSelf(ctxP, iriP->value.s)));

    KjNode* attrNames = kjArray(swRest.kjsonP, "attributeNames");
    KjNode* attrsAgg  = kjLookup(entry, "attrs");
    if (attrsAgg != NULL)
    {
      for (KjNode* aN = attrsAgg->value.firstChildP; aN != NULL; aN = aN->next)
        if (aN->type == KjString)
          kjChildAdd(attrNames, kjString(swRest.kjsonP, NULL, shortOrSelf(ctxP, aN->value.s)));
    }
    kjChildAdd(et, attrNames);

    kjChildAdd(body, et);
  }

  swRest.out.responseTree   = body;
  swRest.out.httpStatusCode = 200;
  return true;
}
