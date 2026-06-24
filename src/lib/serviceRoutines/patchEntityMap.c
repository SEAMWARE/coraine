//
// FILE            patchEntityMap.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// PATCH /ngsi-ld/v1/entityMaps/{entityMapId} — Update EntityMap (§ 5.14.2).
//
// The spec (§ 5.14.2.4) says "Perform an update operation on the target
// EntityMap using the fields specified within then JSON-LD document.
// Any provided output-only fields shall be ignored." In practice the
// only field of an EntityMap that is meaningfully updatable is
// expiresAt — id and type are immutable, and the entityMap / linkedMaps
// members are broker-generated snapshots. This routine accepts
// expiresAt and 400s on anything else rather than silently ignoring,
// so clients get a clear error on typos / misuse.
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp

#include "swRest/SwRestState.h"                      // swRest

#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjLookup.h"                          // kjLookup

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldCheckDateTime.h"                // ldCheckDateTime, ldIsoToNanoseconds
#include "swNgsild/LdEntityMap.h"                    // LdEntityMapStore, LdEntityMap
#include "swNgsild/ldEntityMap.h"                    // ldEntityMapLookup, ldEntityMapSetExpiresAt

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/patchEntityMap.h"          // Own interface



// -----------------------------------------------------------------------------
//
// patchEntityMap -
//
bool patchEntityMap(void)
{
  const char* mapId = swRest.in.wildcard[0];
  KjNode*     bodyP = swRest.in.requestTree;

  if (bodyP->type != KjObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Not a JSON Object",
            "EntityMap fragment must be a JSON object");
    return true;
  }

  //
  // Walk the body — only expiresAt (and @context as a JSON-LD keyword) is
  // accepted. Any other field (id, type, entityMap, linkedMaps, or an
  // unknown name) → 400.
  //
  KjNode* expiresAtP = NULL;
  for (KjNode* fP = bodyP->value.firstChildP; fP != NULL; fP = fP->next)
  {
    if (fP->name == NULL) continue;
    if (fP->name[0] == '@') continue;       // @context and friends

    if (strcmp(fP->name, "expiresAt") == 0)
    {
      expiresAtP = fP;
      continue;
    }

    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Immutable Field",
            "field '%s' cannot be updated on an EntityMap — only expiresAt is mutable", fP->name);
    return true;
  }

  if (expiresAtP == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Mandatory Field Missing",
            "Update EntityMap requires expiresAt in the body");
    return true;
  }

  if (expiresAtP->type != KjString)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Field Value",
            "expiresAt must be an ISO 8601 DateTime string");
    return true;
  }

  if (!ldCheckDateTime(expiresAtP->value.s, NULL))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Field Value",
            "expiresAt is not a valid ISO 8601 DateTime: '%s'", expiresAtP->value.s);
    return true;
  }

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  if (tenantP->entityMapStoreP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "entity map '%s' not found", mapId);
    return true;
  }

  LdEntityMap* mapP = ldEntityMapLookup((LdEntityMapStore*) tenantP->entityMapStoreP, mapId);
  if (mapP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "entity map '%s' not found", mapId);
    return true;
  }

  ldEntityMapSetExpiresAt(mapP, ldIsoToNanoseconds(expiresAtP->value.s));

  swRest.out.httpStatusCode = 204;
  return true;
}
