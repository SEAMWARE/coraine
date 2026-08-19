//
// FILE            patchEntityMap.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// PATCH /ngsi-ld/v1/entityMaps/{entityMapId} — Update EntityMap (§ 5.14.2).
//
// The spec (§ 14.3.4) says "Perform an update operation on the target
// EntityMap using the fields specified within then JSON-LD document.
// Any provided output-only fields shall be ignored."
//
// The output-only members are entityMap and linkedMaps (§ 5.2.6.5.5:
// "They shall not be provided by Context Consumers. In the event that
// they are provided in update operations, NGSI-LD implementations shall
// ignore them."). Both are therefore skipped here, NOT rejected — a
// client that echoes back a retrieved EntityMap must not get a 400.
//
// Ignoring means treating them as absent, so a body carrying nothing but
// output-only members still fails the expiresAt check below.
//
// expiresAt is the only meaningfully updatable field. id and type are not
// output-only, and the spec says nothing about updating them, so they keep
// their 400 — as does any unknown field.
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp

#include "corRest/CorRestState.h"                      // corRest

#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjLookup.h"                          // kjLookup

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/ldCheckDateTime.h"                // ldCheckDateTime, ldIsoToNanoseconds
#include "corNgsild/LdEntityMap.h"                    // LdEntityMapStore, LdEntityMap
#include "corNgsild/ldEntityMap.h"                    // ldEntityMapLookup, ldEntityMapSetExpiresAt

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/patchEntityMap.h"          // Own interface



// -----------------------------------------------------------------------------
//
// patchEntityMap -
//
bool patchEntityMap(void)
{
  const char* mapId = corRest.in.wildcard[0];
  KjNode*     bodyP = corRest.in.requestTree;

  if (bodyP->type != KjObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Not a JSON Object",
            "EntityMap fragment must be a JSON object");
    return true;
  }

  //
  // Walk the body — only expiresAt (and @context as a JSON-LD keyword) is
  // accepted. The output-only members are ignored. Any other field (id,
  // type, or an unknown name) → 400.
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

    // Output-only (§ 5.2.6.5.5) — silently ignored, per § 14.3.4.
    if ((strcmp(fP->name, "entityMap") == 0) || (strcmp(fP->name, "linkedMaps") == 0))
      continue;

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

  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

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

  corRest.out.httpStatusCode = 204;
  return true;
}
