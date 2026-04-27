//
// FILE            getEntityTemporal.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/temporal/entities/{id}
// NGSI-LD § 5.7.4 — Retrieve Temporal Evolution of an Entity.
//
// v1: no filtering — returns all recorded history. timerel/timeAt/
// endTimeAt/q/attrs/options=… come in a follow-up. The plugin's
// TroeQueryFilter is currently empty; we pass NULL.
//

#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/KjNode.h"                            // KjNode

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*

#include "troe/TroeDriver.h"                         // troe

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getEntityTemporal.h"       // Own interface



bool getEntityTemporal(void)
{
  const char* entityId = swRest.in.wildcard[0];

  if (entityId == NULL || entityId[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "missing entity id in URL");
    return true;
  }

  if (troe.entityTemporalRetrieve == NULL)
  {
    ldError(501, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported",
            "Not Implemented",
            "active TRoE plugin does not support temporal queries");
    return true;
  }

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  KjNode* result = NULL;
  int     r      = troe.entityTemporalRetrieve(tenantP, entityId, NULL, &result);

  if (r == TROE_NOT_FOUND || (r == TROE_OK && result == NULL))
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "no temporal data for entity '%s'", entityId);
    return true;
  }

  if (r != TROE_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "temporal retrieve failed for entity '%s'", entityId);
    return true;
  }

  swRest.out.responseTree = result;
  swRest.out.httpStatusCode = 200;
  return true;
}
