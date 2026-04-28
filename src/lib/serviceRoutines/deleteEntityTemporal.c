//
// FILE            deleteEntityTemporal.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// DELETE /ngsi-ld/v1/temporal/entities/{id} — § 5.6.16 / § 6.19.3.2.
// Removes the complete temporal evolution of one entity (both
// troe_entities and troe_attrs rows). Distops are deferred — see
// project_temporal_distops_deferred memory.
//

#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild

#include "troe/TroeDriver.h"                         // troe

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/deleteEntityTemporal.h"    // Own interface



bool deleteEntityTemporal(void)
{
  const char* entityId = swRest.in.wildcard[0];

  if (entityId == NULL || entityId[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "missing entity id in URL");
    return true;
  }

  if (troe.entityTemporalDelete == NULL)
  {
    ldError(501, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported",
            "Not Implemented",
            "active TRoE plugin does not support temporal-entity delete");
    return true;
  }

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  int r = troe.entityTemporalDelete(tenantP, entityId);

  if (r == TROE_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "no temporal data for entity '%s'", entityId);
    return true;
  }
  if (r != TROE_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "temporal-entity delete failed for '%s'", entityId);
    return true;
  }

  swRest.out.httpStatusCode = 204;
  return true;
}
