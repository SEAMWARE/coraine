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
// Filtering supported in this slice:
//   ?timerel + ?timeAt (+ ?endTimeAt for between)
//   ?timeproperty
//   ?attrs (comma list, post-expansion)
//   ?lastN (per-attribute instance cap)
//
// q / geoQ / Content-Range pagination — follow.
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp, memset

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/KjNode.h"                            // KjNode

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild

#include "troe/TroeDriver.h"                         // troe, TroeQueryFilter

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

  // timerel — optional for the single-entity GET (mandatory only for the
  // multi-entity GET /temporal/entities, per § 6.18.3.2). When present,
  // timeAt is mandatory; when timerel=between, endTimeAt is too.
  if (swNgsild.timerel != NULL)
  {
    if (swNgsild.timeAt == NULL)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "missing required URL parameter 'timeAt' (timerel='%s')", swNgsild.timerel);
      return true;
    }
    if (strcmp(swNgsild.timerel, "between") == 0 && swNgsild.endTimeAt == NULL)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "missing required URL parameter 'endTimeAt' for timerel='between'");
      return true;
    }
  }

  if (troe.entityTemporalRetrieve == NULL)
  {
    ldError(501, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported",
            "Not Implemented",
            "active TRoE plugin does not support temporal queries");
    return true;
  }

  TroeQueryFilter filter;
  memset(&filter, 0, sizeof(filter));
  filter.timerel      = swNgsild.timerel;
  filter.timeAtIso    = swNgsild.timeAt;
  filter.endTimeAtIso = swNgsild.endTimeAt;
  filter.timeproperty = swNgsild.timeproperty;
  filter.attrV        = swNgsild.attrsV;
  filter.lastN        = swNgsild.lastN;

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  KjNode* result = NULL;
  int     r      = troe.entityTemporalRetrieve(tenantP, entityId, &filter, &result);

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

  swRest.out.responseTree   = result;
  swRest.out.httpStatusCode = 200;
  return true;
}
