//
// FILE            postEntityTemporalAttrs.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/temporal/entities/{id}/attrs — § 5.6.12 / § 6.20.3.1.
// Add Attributes to Temporal Evolution of an Entity. The body is an
// EntityTemporal Fragment — top-level id/type are optional (the entity
// id is in the URL); each per-attribute array of instances is appended
// to TRoE.
//
// Response: 204 No Content on success, 404 if the entity has no
// existing temporal evolution.
//

#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/KjNode.h"                            // KjNode

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild

#include "troe/TroeDriver.h"                         // troe

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postEntityTemporalAttrs.h" // Own interface



bool postEntityTemporalAttrs(void)
{
  const char* entityId = swRest.in.wildcard[0];
  KjNode*     bodyP    = swRest.in.requestTree;

  if (entityId == NULL || entityId[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "missing entity id in URL");
    return true;
  }
  if (bodyP == NULL || bodyP->type != KjObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "request body must be a JSON-LD object (EntityTemporal Fragment)");
    return true;
  }

  if (troe.entityTemporalAttrsAdd == NULL)
  {
    ldError(501, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported",
            "Not Implemented",
            "active TRoE plugin does not support add-attrs on temporal entities");
    return true;
  }

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  int r = troe.entityTemporalAttrsAdd(tenantP, entityId, bodyP);

  if (r == TROE_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "no temporal evolution for entity '%s'", entityId);
    return true;
  }
  if (r != TROE_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "temporal add-attrs failed for '%s'", entityId);
    return true;
  }

  swRest.out.httpStatusCode = 204;
  return true;
}
