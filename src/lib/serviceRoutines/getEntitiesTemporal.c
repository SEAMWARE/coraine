//
// FILE            getEntitiesTemporal.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/temporal/entities
// NGSI-LD § 5.7.4 — Query Temporal Evolution of Entities (§ 6.18.3.2).
//
// Filtering supported in this slice:
//   ?id (CSV) / ?idPattern / ?type (CSV)        — entity selectors
//   ?timerel + ?timeAt (+ ?endTimeAt for between, mandatory)
//   ?timeproperty                                — observedAt by default
//   ?attrs                                       — attribute name filter
//   ?q                                           — q-tree compiled to SQL
//   ?lastN                                       — per-attr instance cap
//   ?limit / ?offset                             — pagination
//
// Distops are deferred to a later phase covering all eight temporal
// ops together — see project_temporal_distops_deferred memory. This
// route is local-only for now even when ?local is absent.
//

#include <stddef.h>                                  // NULL
#include <stdio.h>                                   // snprintf
#include <string.h>                                  // strcmp, memset

#include "swRest/SwRestState.h"                      // swRest
#include "swRest/swRestOutHeader.h"                  // swRestOutHeaderAdd
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild

#include "troe/TroeDriver.h"                         // troe, TroeQueryFilter, TroeRangeInfo
#include "troe/troeQTreeToSql.h"                     // troeQTreeToSql

#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getEntitiesTemporal.h"     // Own interface



bool getEntitiesTemporal(void)
{
  // § 6.18.3.2: timerel is mandatory on the multi-entity GET (unlike the
  // single-entity retrieve, where it's optional). When present, timeAt
  // is mandatory; for timerel=between, endTimeAt is too.
  if (swNgsild.timerel == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "missing required URL parameter 'timerel'");
    return true;
  }
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

  // § 6.18.3.2: at least one of (id, idPattern, type, attrs, q, georel)
  // must be present. attrs is also a synonym for pick+q in this route's
  // table — the deprecated combined form. For this slice we just enforce
  // the simple "one of" rule.
  if (swNgsild.idV == NULL && swNgsild.idPattern == NULL && swNgsild.typeV == NULL
      && swNgsild.attrsV == NULL && swNgsild.qExpr == NULL && swNgsild.georel == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "at least one of 'id', 'idPattern', 'type', 'attrs', 'q', 'georel' must be supplied");
    return true;
  }

  if (troe.entityTemporalQuery == NULL)
  {
    ldError(501, "https://uri.etsi.org/ngsi-ld/errors/OperationNotSupported",
            "Not Implemented",
            "active TRoE plugin does not support multi-entity temporal queries");
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
  filter.idV          = swNgsild.idV;
  filter.idPattern    = swNgsild.idPattern;
  filter.typeV        = swNgsild.typeV;
  filter.limit        = swNgsild.limit;
  filter.offset       = swNgsild.offset;

  if (swNgsild.qExpr != NULL)
    filter.qSqlPredicate = troeQTreeToSql(swNgsild.qExpr, &swRest.kalloc);

  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  TroeRangeInfo rangeInfo;
  memset(&rangeInfo, 0, sizeof(rangeInfo));

  KjNode* result = NULL;
  int     r      = troe.entityTemporalQuery(tenantP, &filter, &result, &rangeInfo);

  if (r != TROE_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "temporal query failed");
    return true;
  }

  // No matches → 200 + empty array (per § 6.18.3.2 query semantics).
  if (result == NULL)
    result = kjArray(swRest.kjsonP, NULL);

  swRest.out.responseTree = result;

  // § 6.3.10: 206 Partial Content + Content-Range when any entity was truncated.
  // The bounds span the union of all entities' attribute time ranges in the
  // response — there's only one Content-Range header per HTTP response.
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
