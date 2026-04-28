//
// FILE            postEntitiesTemporal.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/temporal/entities — § 5.6.11 / § 6.18.3.1.
// Create or Update Temporal Evolution of an Entity. The body is an
// EntityTemporal — id, type, and per-attribute arrays of instances.
// We delegate to the plugin which inserts directly into the TRoE
// store, bypassing the current-state DB.
//
// Response: 201 + Location on create. 204 on update of an existing
// temporal evolution (entity already had rows in TRoE — we still
// just append, idempotent on PK collision).
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strlen, strcpy, strcat

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/swRestOutHeader.h"                  // swRestOutHeaderAdd
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjLookup.h"                          // kjLookup
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild

#include "troe/TroeDriver.h"                         // troe

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postEntitiesTemporal.h"    // Own interface



bool postEntitiesTemporal(void)
{
  KjNode* bodyP = swRest.in.requestTree;

  if (bodyP == NULL || bodyP->type != KjObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "request body must be a JSON-LD object (EntityTemporal)");
    return true;
  }

  KjNode* idP   = kjLookup(bodyP, "id");
  KjNode* typeP = kjLookup(bodyP, "type");

  if (idP == NULL || idP->type != KjString || idP->value.s[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "EntityTemporal must include a non-empty 'id'");
    return true;
  }
  if (typeP == NULL || typeP->type != KjString || typeP->value.s[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "EntityTemporal must include a non-empty 'type'");
    return true;
  }

  if (troe.entityTemporalCreate == NULL)
  {
    ldError(501, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported",
            "Not Implemented",
            "active TRoE plugin does not support temporal-entity create");
    return true;
  }

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  int r = troe.entityTemporalCreate(tenantP, bodyP);

  if (r != TROE_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "temporal-entity create failed");
    return true;
  }

  // 201 Created with Location header. The spec also defines 204 (update)
  // for the case where the temporal evolution already existed — for v1
  // we don't distinguish; the plugin's INSERT-ON-CONFLICT-DO-NOTHING
  // makes both cases observably 201.
  const char* prefix = "/ngsi-ld/v1/temporal/entities/";
  int   locLen = (int) strlen(prefix) + (int) strlen(idP->value.s) + 1;
  char* locBuf = (char*) kaAlloc(&swRest.kalloc, locLen);
  strcpy(locBuf, prefix);
  strcat(locBuf, idP->value.s);
  swRestOutHeaderAdd("Location", locBuf);

  swRest.out.httpStatusCode = 201;
  return true;
}
