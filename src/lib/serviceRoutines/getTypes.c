//
// FILE            getTypes.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
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

#include "corRest/CorRestState.h"                       // corRest
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeArray, corTreeObject, corTreeString, corTreeChildAdd
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corTree/corTreeClone.h"                     // corTreeClone

#include "corJsonld/corLdCompact.h"                   // corLdCompact
#include "corJsonld/corLdInit.h"                      // corLdCoreContext

#include "corNgsild/corNgsild.h"                        // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdVocab.h"                         // LD_VOCAB_*
#include "corNgsild/LdRegCache.h"                      // LdRegCache
#include "corNgsild/ldDiscovery.h"                     // ldDiscoveryRegAugmentTypes
#include "corNgsild/ldDiscoveryForward.h"              // ldDiscoveryForwardTypes, ldDiscoveryShouldForward
#include "corNgsild/ldCsourceAlias.h"                  // ldCsourceAliasForTenant

#include "db/DbDriver.h"                              // db, DB_OK
#include "db/Tenant.h"                                // Tenant

#include "serviceRoutines/discoveryIri.h"             // discoveryIri
#include "serviceRoutines/getTypes.h"                 // Own interface



// -----------------------------------------------------------------------------
//
// shortOrSelf -
//
static const char* shortOrSelf(CorLdContext* ctxP, const char* iri)
{
  const char* compact = corLdCompact(ctxP, iri);
  return (compact != NULL) ? compact : iri;
}



// -----------------------------------------------------------------------------
//
// getTypes -
//
bool getTypes(void)
{
  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

  if (db.typeList == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
            "type discovery not supported by this DB plugin");
    return true;
  }

  bool details = corNgsild.details;

  CorNode* aggregated = NULL;
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
  if (!corNgsild.local && tenantP->regCacheP != NULL)
    ldDiscoveryRegAugmentTypes(aggregated, (LdRegCache*) tenantP->regCacheP, details);

  //
  // Mode 3 only (default — !local && !noForward): forward the query to
  // every CSR supporting retrieveEntityType(s) and merge the results.
  //
  if (!corNgsild.local && !corNgsild.noForward &&
      tenantP->regCacheP != NULL &&
      ldDiscoveryShouldForward())
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);
    ldDiscoveryForwardTypes(aggregated, (LdRegCache*) tenantP->regCacheP, details, ownAlias);
  }

  CorLdContext* ctxP = (corNgsild.contextP != NULL) ? corNgsild.contextP : corLdCoreContext();

  if (!details)
  {
    //
    // EntityTypeList (§ 5.2.24): { id, type:"EntityTypeList", typeList:[short names] }
    //
    CorNode* body = corTreeObject(corRest.kallocP, NULL);
    corTreeChildAdd(body, corTreeString(corRest.kallocP, "id", "urn:ngsi-ld:EntityTypeList:local"));
    corTreeChildAdd(body, corTreeString(corRest.kallocP, "type", "EntityTypeList"));

    CorNode* typeList = corTreeArray(corRest.kallocP, "typeList");
    for (CorNode* entry = aggregated->value.head; entry != NULL; entry = entry->next)
    {
      CorNode* iriP = corTreeLookup(entry, "typeIri");
      if (iriP == NULL || iriP->type != CorString) continue;
      corTreeChildAdd(typeList, corTreeString(corRest.kallocP, NULL, shortOrSelf(ctxP, iriP->value.s)));
    }
    corTreeChildAdd(body, typeList);

    corRest.out.responseTree   = body;
    corRest.out.httpStatusCode = 200;
    return true;
  }

  //
  // Details: EntityType[] (§ 5.2.25) — array of
  //   { id: <IRI>, type:"EntityType", typeName:<short>, attributeNames:[<short>] }
  //
  CorNode* body = corTreeArray(corRest.kallocP, NULL);

  for (CorNode* entry = aggregated->value.head; entry != NULL; entry = entry->next)
  {
    CorNode* iriP = corTreeLookup(entry, "typeIri");
    if (iriP == NULL || iriP->type != CorString) continue;

    CorNode* et = corTreeObject(corRest.kallocP, NULL);
    corTreeChildAdd(et, corTreeString(corRest.kallocP, "id", discoveryIri(iriP->value.s)));
    corTreeChildAdd(et, corTreeString(corRest.kallocP, "type", "EntityType"));
    corTreeChildAdd(et, corTreeString(corRest.kallocP, "typeName", shortOrSelf(ctxP, iriP->value.s)));

    CorNode* attrNames = corTreeArray(corRest.kallocP, "attributeNames");
    CorNode* attrsAgg = corTreeLookup(entry, "attrs");
    if (attrsAgg != NULL)
    {
      for (CorNode* aN = attrsAgg->value.head; aN != NULL; aN = aN->next)
        if (aN->type == CorString)
          corTreeChildAdd(attrNames, corTreeString(corRest.kallocP, NULL, shortOrSelf(ctxP, aN->value.s)));
    }
    corTreeChildAdd(et, attrNames);

    corTreeChildAdd(body, et);
  }

  corRest.out.responseTree   = body;
  corRest.out.httpStatusCode = 200;
  return true;
}
