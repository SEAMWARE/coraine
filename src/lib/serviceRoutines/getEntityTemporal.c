//
// FILE            getEntityTemporal.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/temporal/entities/{id}
// NGSI-LD § 5.7.3 — Retrieve Temporal Evolution of an Entity (§ 6.19.3.1).
//
// Filtering supported in this slice:
//   ?timerel + ?timeAt (+ ?endTimeAt for between)
//   ?timeproperty
//   ?attrs (comma list, post-expansion)
//   ?lastN (per-attribute instance cap)
//
// Content-Range pagination — follows. ?q= is NOT in the spec's URL-param
// table for this route (§ 6.19.3.1) — it lives only on the multi-entity
// Query Temporal Evolution (§ 6.18.3.2).
//

#include <stddef.h>                                  // NULL
#include <stdio.h>                                   // snprintf
#include <string.h>                                  // strcmp, memset

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/swRestOutHeader.h"                  // swRestOutHeaderAdd
#include "kjson/KjNode.h"                            // KjNode
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldPickOmit.h"                     // ldPickOmit
#include "swNgsild/ldToTemporalValues.h"             // ldToTemporalValues

#include "troe/TroeDriver.h"                         // troe, TroeQueryFilter, TroeRangeInfo

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
  filter.datasetIdV   = swNgsild.datasetIdV;

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  TroeRangeInfo rangeInfo;
  memset(&rangeInfo, 0, sizeof(rangeInfo));

  KjNode* result = NULL;
  int     r      = troe.entityTemporalRetrieve(tenantP, entityId, &filter, &result, &rangeInfo);

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

  // § 4.5.4 / § 4.5.5: pick/omit attribute projection (lang reduction
  // and ?format=temporalValues run in renderHook, see ldHooks.c).
  if (swNgsild.pickV != NULL || swNgsild.omitV != NULL)
    ldPickOmit(result, swNgsild.pickV, swNgsild.omitV);

  swRest.out.responseTree = result;

  // § 6.3.10: 206 Partial Content + Content-Range when truncated.
  if (rangeInfo.truncated && rangeInfo.rangeStartIso != NULL && rangeInfo.rangeEndIso != NULL)
  {
    int   sz  = 96;
    char* buf = (char*) kaAlloc(&swRest.kalloc, sz);
    if (rangeInfo.size > 0)
      snprintf(buf, sz, "DateTime %s-%s/%d", rangeInfo.rangeStartIso, rangeInfo.rangeEndIso, rangeInfo.size);
    else
      snprintf(buf, sz, "DateTime %s-%s/*", rangeInfo.rangeStartIso, rangeInfo.rangeEndIso);
    swRestOutHeaderAdd("Content-Range", buf);
    swRest.out.httpStatusCode = 206;
  }
  else
    swRest.out.httpStatusCode = 200;

  return true;
}
