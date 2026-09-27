//
// FILE            timescaleQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Read path: single-entity retrieve and multi-entity query.
//
// § 6.4.7.3 temporal pagination: the per-attribute page limit is
// ?lastN (descending) or ?firstN (ascending) or the configured default
// (ascending); ?offsetN skips into the sequence. When instances remain
// beyond the returned page, TroeRangeInfo->hasMore is set so the
// service routine emits Link rel="intervalafter"/"intervalbefore"
// page pointers.
//
// Result tree shape (NGSI-LD § 5.7.4 TemporalEntity):
//   {
//     "id": "urn:V1",
//     "type": "Vehicle",
//     "speed": [
//        { "type": "Property", "value": 42, "observedAt": "...", "modifiedAt": "..." },
//        { "type": "Property", "value": 99, "observedAt": "...", "modifiedAt": "..." }
//     ],
//     "owner": [
//        { "type": "Relationship", "object": "urn:Person:1", "modifiedAt": "..." }
//     ]
//   }
//

#include <stddef.h>                                       // NULL
#include <stdint.h>                                       // uint64_t
#include <stdio.h>                                        // snprintf
#include <string.h>                                       // strcmp, strlen, memset
#include <stdlib.h>                                       // strtol, strtoll, strtod
#include <math.h>                                         // sqrt
#include <time.h>                                         // gmtime_r, time_t
#include <libpq-fe.h>                                     // PG*

#include "corLog/corLog.h"                                // COR_E
#include "corTree/corTreeBuilder.h"                       // corTreeObject, corTreeArray, corTreeString, corTreeInteger, corTreeFloat, corTreeBoolean, corTreeChildAdd
#include "corTree/corTreeLookup.h"                        // corTreeLookup
#include "corJson/corJsonParse.h"                         // corJsonParse
#include "corAlloc/corAlloc.h"                            // corAlloc
#include "corAlloc/corAllocStrdup.h"                      // corAllocStrdup
#include "corAlloc/CorAlloc.h"                            // CorAlloc

#include "corRest/CorRestState.h"                           // corRest
#include "corNgsild/LdAttrType.h"                          // LdAttr*
#include "corNgsild/LdGeoRel.h"                            // LdGeoNear, LdGeoRelType
#include "corNgsild/CorNgsild.h"                            // corNgsild

#include "troe/TroeDriver.h"                              // TroeQueryFilter, TROE_*

#include "temporal/timescale/timescaleGlobals.h"          // timescaleConn
#include "temporal/timescale/timescalePool.h"             // timescaleConnGet, timescaleConnRelease
#include "temporal/timescale/timescaleQuery.h"            // Own interface


// Default per-attribute page limit when neither ?firstN nor ?lastN is
// given (§ 6.4.7.3 "default maximum limit"). Configurable later via CLI;
// hardcoded for now so tests get a stable knob.
#define TROE_DEFAULT_INSTANCE_CAP 100



// -----------------------------------------------------------------------------
//
// kindToTypeString -
//
static const char* kindToTypeString(int kind)
{
  switch (kind)
  {
    case LdAttrProperty:         return "Property";
    case LdAttrRelationship:     return "Relationship";
    case LdAttrGeoProperty:      return "GeoProperty";
    case LdAttrLanguageProperty: return "LanguageProperty";
    case LdAttrVocabProperty:    return "VocabProperty";
    case LdAttrListProperty:     return "ListProperty";
    case LdAttrListRelationship: return "ListRelationship";
    case LdAttrJsonProperty:     return "JsonProperty";
    default:                     return "Property";
  }
}



// -----------------------------------------------------------------------------
//
// kindValueFieldName - which JSON field holds the value for this attr-kind.
//
static const char* kindValueFieldName(int kind)
{
  switch (kind)
  {
    case LdAttrRelationship:     return "object";
    case LdAttrLanguageProperty: return "languageMap";
    case LdAttrVocabProperty:    return "vocab";
    case LdAttrListProperty:     return "valueList";
    case LdAttrListRelationship: return "objectList";
    case LdAttrJsonProperty:     return "json";
    default:                     return "value";
  }
}



// -----------------------------------------------------------------------------
//
// makeValueNode - build the value-bearing field from typed columns.
//
static CorNode* makeValueNode(CorJson* corJsonP, const char* vfn,
                             const char* v_text, const char* v_number,
                             const char* v_bool, const char* v_compnd)
{
  if (v_compnd != NULL && v_compnd[0] != 0)
  {
    char*   dup    = corAllocStrdup(&corRest.kalloc, v_compnd);
    CorNode* parsed = corJsonParse(corJsonP, dup);
    if (parsed != NULL)
    {
      parsed->name = (char*) vfn;
      return parsed;
    }
  }
  if (v_number != NULL)
  {
    double n = strtod(v_number, NULL);
    if ((double)(long long) n == n)
      return corTreeInteger(corJsonP->kallocP, vfn, (long long) n);
    return corTreeFloat(corJsonP->kallocP, vfn, n);
  }
  if (v_bool != NULL)
    return corTreeBoolean(corJsonP->kallocP, vfn, (v_bool[0] == 't') ? true : false);
  if (v_text != NULL)
    return corTreeString(corJsonP->kallocP, vfn, corAllocStrdup(&corRest.kalloc, v_text));
  return NULL;
}



// -----------------------------------------------------------------------------
//
// stripZeroMs - trim an all-zero fractional part so a clean second renders
// without the artificial sub-second padding (matches the canonical fixtures used
// by ETSI's temporal tests). The buffer is in-place rewritable: postgres'
// to_char output lives in PQgetvalue's libpq-owned storage, so we corAllocStrdup
// first, then trim. Caller passes in the strdup'd copy.
//
// Both widths are handled: created_at/modified_at render with microseconds (.US)
// to match what the core API returns for the same entity, observed_at still with
// milliseconds (.MS), so ".000000Z" and ".000Z" both reach here.
//
static char* stripZeroMs(char* s)
{
  if (s == NULL) return NULL;
  size_t n = strlen(s);

  if (n >= 8 && memcmp(s + n - 8, ".000000Z", 8) == 0)
  {
    s[n - 8] = 'Z';
    s[n - 7] = '\0';
  }
  else if (n >= 5 && memcmp(s + n - 5, ".000Z", 5) == 0)
  {
    s[n - 5] = 'Z';
    s[n - 4] = '\0';
  }

  return s;
}



// -----------------------------------------------------------------------------
//
// timeColumn - column-name for the requested ?timeproperty.
//
static const char* timeColumn(const char* timeProp)
{
  if (timeProp == NULL)                      return "observed_at";
  if (strcmp(timeProp, "observedAt") == 0)   return "observed_at";
  if (strcmp(timeProp, "createdAt") == 0)    return "modified_at";
  if (strcmp(timeProp, "modifiedAt") == 0)   return "modified_at";
  if (strcmp(timeProp, "deletedAt") == 0)    return "modified_at";
  return "observed_at";
}



// -----------------------------------------------------------------------------
//
// attrsInClause - " AND attr_name IN ('a','b',...)" or "" when attrV is NULL.
//
static const char* attrsInClause(char** attrV, CorAlloc* kaP)
{
  if (attrV == NULL || attrV[0] == NULL)
    return "";

  int needed = 32;
  for (int i = 0; attrV[i] != NULL; i++)
    needed += (int) strlen(attrV[i]) * 2 + 4;

  char* buf = (char*) corAlloc(kaP, needed);
  int   p   = 0;
  p += snprintf(buf + p, needed - p, " AND attr_name IN (");

  for (int i = 0; attrV[i] != NULL; i++)
  {
    if (i > 0) { buf[p++] = ','; }
    buf[p++] = '\'';
    for (const char* s = attrV[i]; *s; s++)
    {
      if (*s == '\'') { buf[p++] = '\''; buf[p++] = '\''; }
      else            buf[p++] = *s;
    }
    buf[p++] = '\'';
  }
  buf[p++] = ')';
  buf[p]   = 0;
  return buf;
}



// -----------------------------------------------------------------------------
//
// datasetIdsInClause - " AND dataset_id IN ('','urn:..',..)" or "" when empty.
//
// "@none" in the URL param maps to the empty-string dataset_id we store for
// the default instance.
//
static const char* datasetIdsInClause(char** dsV, CorAlloc* kaP)
{
  if (dsV == NULL || dsV[0] == NULL)
    return "";

  int needed = 32;
  for (int i = 0; dsV[i] != NULL; i++)
    needed += (int) strlen(dsV[i]) * 2 + 4;

  char* buf = (char*) corAlloc(kaP, needed);
  int   p   = 0;
  p += snprintf(buf + p, needed - p, " AND dataset_id IN (");

  for (int i = 0; dsV[i] != NULL; i++)
  {
    if (i > 0) { buf[p++] = ','; }
    buf[p++] = '\'';
    if (strcmp(dsV[i], "@none") != 0)
    {
      for (const char* s = dsV[i]; *s; s++)
      {
        if (*s == '\'') { buf[p++] = '\''; buf[p++] = '\''; }
        else            buf[p++] = *s;
      }
    }
    buf[p++] = '\'';
  }
  buf[p++] = ')';
  buf[p]   = 0;
  return buf;
}



// -----------------------------------------------------------------------------
//
// runQPreconditionLocked - returns true if the entity matches q, false if not.
// On DB error sets *errOut to true. NULL qPred → matches.
// Operates on the thread-local timescaleConn.
//
static bool runQPreconditionLocked(const char* qPred, const char* entityId,
                                   bool* errOut)
{
  if (qPred == NULL)
    return true;

  // troeQTreeToSql emits $1 for entity_id inside every EXISTS-subquery, so the
  // caller binds that one param here (the connection identifies the tenant).
  const char* idParam[1] = { entityId };
  int   sz  = (int) strlen(qPred) + 32;
  char* sql = (char*) corAlloc(&corRest.kalloc, sz);
  snprintf(sql, sz, "SELECT %s", qPred);

  PGresult* res = PQexecParams(timescaleConn, sql, 1, NULL, idParam, NULL, NULL, 0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    COR_E("timescale: q precondition SELECT failed: %s", PQerrorMessage(timescaleConn));
    PQclear(res);
    *errOut = true;
    return false;
  }
  bool matches = (PQntuples(res) == 1
                  && PQgetvalue(res, 0, 0) != NULL
                  && PQgetvalue(res, 0, 0)[0] == 't');
  PQclear(res);
  return matches;
}




// -----------------------------------------------------------------------------
//
// typeNodeFromJson - build the entity's "type" member from the JSON array the
// SELECT returns for the TEXT[] column, e.g. ["Vehicle","Car"].
//
// § 5.2.6.4.2: one type name renders as a JSON string, several as an array. A
// missing or empty list yields NULL and the member is left out entirely, which
// is what an Entity written before the column became an array looks like.
//
static CorNode* typeNodeFromJson(const char* json, CorJson* corJsonP, CorAlloc* kaP)
{
  if ((json == NULL) || (json[0] == 0))
    return NULL;

  char*   copy  = corAllocStrdup(kaP, json);
  CorNode* arrayP = corJsonParse(corJsonP, copy);

  if ((arrayP == NULL) || (arrayP->type != CorArray) || (arrayP->value.head == NULL))
    return NULL;

  CorNode* firstP = arrayP->value.head;

  if (firstP->next == NULL)
  {
    if ((firstP->type != CorString) || (firstP->value.s == NULL) || (firstP->value.s[0] == 0))
      return NULL;
    return corTreeString(corJsonP->kallocP, "type", firstP->value.s);
  }

  arrayP->name = (char*) "type";
  return arrayP;
}



// -----------------------------------------------------------------------------
//
// buildEntityTemporalDocLocked - build the EntityTemporal tree for one entity.
//
// Mutex must be held. Returns:
//   TROE_OK         — *treePP set to the built tree
//   TROE_NOT_FOUND  — entity has no temporal data (or q didn't match)
//   TROE_ERR        — DB error
//
// entityType is optional; when NULL, the helper looks it up via troe_entities.
//
static int buildEntityTemporalDocLocked(const char* entityId,
                                        const char* entityTypeIn,
                                        TroeQueryFilter* fP, CorNode** treePP,
                                        TroeRangeInfo* rangeOut)
{
  *treePP = NULL;

  const char* timerel       = (fP != NULL) ? fP->timerel       : NULL;
  const char* timeAt        = (fP != NULL) ? fP->timeAtIso     : NULL;
  const char* endTimeAt     = (fP != NULL) ? fP->endTimeAtIso  : NULL;
  const char* timeProp      = (fP != NULL) ? fP->timeproperty  : NULL;
  char**      attrV         = (fP != NULL) ? fP->attrV         : NULL;
  char**      datasetIdV    = (fP != NULL) ? fP->datasetIdV    : NULL;
  int         lastN         = (fP != NULL) ? fP->lastN         : 0;
  int         firstN        = (fP != NULL) ? fP->firstN        : 0;
  int         offsetN       = (fP != NULL) ? fP->offsetN       : 0;
  const char* qPred         = (fP != NULL) ? fP->qSqlPredicate : NULL;
  int         instanceCap   = (fP != NULL && fP->instanceCap > 0) ? fP->instanceCap
                              : (timescaleInstanceCap > 0 ? timescaleInstanceCap : TROE_DEFAULT_INSTANCE_CAP);

  const char* tCol          = timeColumn(timeProp);
  // § 5.7.3 / § 4.5.x — when timeproperty selects a system temporal
  // property, the query is restricted to the rows that *carry* that
  // property: createdAt → only the creation row, deletedAt → only the
  // deletion row. modifiedAt and observedAt match every row.
  const char* opPred = "";
  if (timeProp != NULL && strcmp(timeProp, "createdAt") == 0)
    opPred = " AND op = 'created'";
  else if (timeProp != NULL && strcmp(timeProp, "deletedAt") == 0)
    opPred = " AND op = 'deleted'";
  const char* attrPred      = attrsInClause(attrV, &corRest.kalloc);
  const char* dsPred        = datasetIdsInClause(datasetIdV, &corRest.kalloc);

  // q precondition.
  bool qErr = false;
  if (!runQPreconditionLocked(qPred, entityId, &qErr))
    return qErr ? TROE_ERR : TROE_NOT_FOUND;

  // Entity type — caller may pass it in (multi-entity case fetches in one
  // go); single-entity look-up here.
  const char* entityType = entityTypeIn;
  if (entityType == NULL)
  {
    const char* idParam[1] = { entityId };
    PGresult* eRes = PQexecParams(timescaleConn,
      // array_to_json so the TEXT[] comes back as ["Vehicle","Car"], which
      // corJsonParse turns straight into the node the renderer wants.
      "SELECT array_to_json(entity_type)::text FROM troe_entities "
      "WHERE entity_id = $1 "
      "ORDER BY modified_at DESC LIMIT 1",
      1, NULL, idParam, NULL, NULL, 0);
    if (PQresultStatus(eRes) != PGRES_TUPLES_OK)
    {
      COR_E("timescale: troe_entities SELECT failed: %s", PQerrorMessage(timescaleConn));
      PQclear(eRes);
      return TROE_ERR;
    }
    if (PQntuples(eRes) > 0)
      entityType = corAllocStrdup(&corRest.kalloc, PQgetvalue(eRes, 0, 0));
    PQclear(eRes);
  }

  // Attribute query.
  // $1 = entity_id (the connection identifies the tenant). timeAt/endTimeAt
  // occupy $2 / $3 when set.
  const char* timePred = "";
  int         nParams  = 1;
  const char* paramV[3];
  paramV[0] = entityId;

  if (timerel != NULL)
  {
    // § 4.11.4 timerel bounds:
    //   before  — timeAt is EXCLUSIVE (strict less-than)
    //   after   — timeAt is INCLUSIVE (≥)
    //   between — [timeAt, endTimeAt): lower inclusive, upper exclusive
    if (strcmp(timerel, "before") == 0)
    {
      char* buf = (char*) corAlloc(&corRest.kalloc, 64);
      snprintf(buf, 64, " AND %s < $2::timestamptz", tCol);
      timePred = buf;
      paramV[1] = timeAt;
      nParams   = 2;
    }
    else if (strcmp(timerel, "after") == 0)
    {
      char* buf = (char*) corAlloc(&corRest.kalloc, 64);
      snprintf(buf, 64, " AND %s >= $2::timestamptz", tCol);
      timePred = buf;
      paramV[1] = timeAt;
      nParams   = 2;
    }
    else if (strcmp(timerel, "between") == 0)
    {
      char* buf = (char*) corAlloc(&corRest.kalloc, 96);
      snprintf(buf, 96, " AND %s >= $2::timestamptz AND %s < $3::timestamptz", tCol, tCol);
      timePred = buf;
      paramV[1] = timeAt;
      paramV[2] = endTimeAt;
      nParams   = 3;
    }
  }

  const char* selectCols =
    "attr_name, attr_kind, dataset_id, "
    "to_char(modified_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS modified_at_iso, "
    "to_char(observed_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS observed_at_iso, "
    "op, v_text, v_number, v_bool, v_compound, sub_attrs, "
    "modified_at, observed_at, instance_id, "
    "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') AS created_at_iso";

  // § 6.4.7.3 temporal pagination: the per-attribute page limit N is
  // lastN (descending order) or firstN (ascending) or the configured
  // default limit (ascending); offsetN skips that many instances per
  // attribute in the chosen direction. The limit applies per
  // (attr_name, dataset_id) partition — the old v1.9.1 § 6.3.10
  // total-cap-with-attribute-boundary semantics are gone.
  int         pageLimit = (lastN > 0) ? lastN : ((firstN > 0) ? firstN : instanceCap);
  bool        backwards = (lastN > 0);
  const char* orderDir  = backwards ? "DESC" : "ASC";

  // Pre-pass: per-(attr_name, dataset_id) instance counts within the
  // window — used to detect whether instances remain beyond the current
  // page (hasMore → Link rel="intervalafter"/"intervalbefore" at the
  // API surface).
  char groupSql[4096];
  snprintf(groupSql, sizeof(groupSql),
    "SELECT attr_name, dataset_id, COUNT(*)::int "
    "FROM troe_attrs WHERE entity_id = $1%s%s%s%s "
    "GROUP BY attr_name, dataset_id",
    timePred, opPred, attrPred, dsPred);

  PGresult* gRes = PQexecParams(timescaleConn, groupSql, nParams, NULL, paramV, NULL, NULL, 0);
  if (PQresultStatus(gRes) != PGRES_TUPLES_OK)
  {
    COR_E("timescale: troe_attrs group-pre-pass failed: %s", PQerrorMessage(timescaleConn));
    PQclear(gRes);
    return TROE_ERR;
  }

  int  groupN  = PQntuples(gRes);
  bool hasMore = false;
  for (int i = 0; i < groupN; i++)
  {
    int actual = (int) strtol(PQgetvalue(gRes, i, 2), NULL, 10);
    if (actual > offsetN + pageLimit)
      hasMore = true;
  }
  PQclear(gRes);

  if (groupN == 0 && entityType == NULL)
    return TROE_NOT_FOUND;

  int   sqlSize = 8192;
  char* sql     = (char*) corAlloc(&corRest.kalloc, sqlSize);

  // Per-partition page clip. The window function ORDER BY follows the
  // pagination direction so rn=1 is the first instance of the page
  // sequence (earliest for ascending, latest for descending).
  char rnClip[96];
  snprintf(rnClip, sizeof(rnClip), "rn > %d AND rn <= %d", offsetN, offsetN + pageLimit);

  snprintf(sql, sqlSize,
    "SELECT * FROM ("
    "SELECT %s, "
    "       ROW_NUMBER() OVER (PARTITION BY attr_name, dataset_id ORDER BY %s %s) AS rn "
    "FROM troe_attrs "
    "WHERE entity_id = $1%s%s%s%s) sub "
    "WHERE %s "
    "ORDER BY attr_name, dataset_id, %s %s",
    selectCols, tCol, orderDir, timePred, opPred, attrPred, dsPred, rnClip, tCol, orderDir);

  PGresult* aRes = PQexecParams(timescaleConn, sql, nParams, NULL, paramV, NULL, NULL, 0);

  if (PQresultStatus(aRes) != PGRES_TUPLES_OK)
  {
    COR_E("timescale: troe_attrs SELECT failed: %s", PQerrorMessage(timescaleConn));
    PQclear(aRes);
    return TROE_ERR;
  }

  int  rowN      = PQntuples(aRes);

  if (rowN == 0 && entityType == NULL)
  {
    PQclear(aRes);
    return TROE_NOT_FOUND;
  }

  CorJson* corJsonP = corRest.corJsonP;
  CorNode* root  = corTreeObject(corRest.kallocP, NULL);

  corTreeChildAdd(root, corTreeString(corRest.kallocP, "id", entityId));

  CorNode* typeNodeP = typeNodeFromJson(entityType, corJsonP, &corRest.kalloc);
  if (typeNodeP != NULL)
    corTreeChildAdd(root, typeNodeP);

  // § 4.5.2 / § 4.5.6 createdAt / modifiedAt at the entity level — derived
  // from troe_entities. createdAt = the modified_at of the earliest 'created'
  // row; modifiedAt = the most recent modified_at across all rows (which
  // also covers append / replace updates). Attached unconditionally; the
  // common renderHook strips them again when ?sysAttrs is not set, so only
  // sysAttrs-aware consumers (incl. ldOrderSort orderBy=createdAt /
  // modifiedAt) actually see them.
  {
    const char* idParam[1] = { entityId };
    PGresult* tRes = PQexecParams(timescaleConn,
      "SELECT to_char(MIN(modified_at) FILTER (WHERE op = 'created') "
      "       AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), "
      "       to_char(MAX(modified_at) AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), "
      "       (ARRAY_AGG(op ORDER BY modified_at DESC))[1] "
      "FROM troe_entities WHERE entity_id = $1",
      1, NULL, idParam, NULL, NULL, 0);
    if (PQresultStatus(tRes) == PGRES_TUPLES_OK && PQntuples(tRes) > 0)
    {
      if (!PQgetisnull(tRes, 0, 0))
        corTreeChildAdd(root, corTreeString(corRest.kallocP, "createdAt",
                                   stripZeroMs(corAllocStrdup(&corRest.kalloc, PQgetvalue(tRes, 0, 0)))));
      if (!PQgetisnull(tRes, 0, 1))
        corTreeChildAdd(root, corTreeString(corRest.kallocP, "modifiedAt",
                                   stripZeroMs(corAllocStrdup(&corRest.kalloc, PQgetvalue(tRes, 0, 1)))));

      //
      // § 5.2.6.2: an Entity's deletedAt is used "in the temporal
      // representation of Entities". When the Entity's most recent row is its
      // deletion, that row's time - the same as modifiedAt above. Not a sysAttr
      // to strip: like an instance's deletedAt, it is what says the thing is gone.
      //
      if ((!PQgetisnull(tRes, 0, 1)) && (!PQgetisnull(tRes, 0, 2)) && (strcmp(PQgetvalue(tRes, 0, 2), "deleted") == 0))
        corTreeChildAdd(root, corTreeString(corRest.kallocP, "deletedAt",
                                   stripZeroMs(corAllocStrdup(&corRest.kalloc, PQgetvalue(tRes, 0, 1)))));
    }
    PQclear(tRes);
  }

  // Track min/max ts (in tCol axis) for Content-Range. ISO strings sort
  // lexically so straight strcmp is fine.
  const char* minIso = NULL;
  const char* maxIso = NULL;

  for (int r = 0; r < rowN; r++)
  {
    const char* attrName  = PQgetvalue(aRes, r, 0);
    int         attrKind  = (int) strtol(PQgetvalue(aRes, r, 1), NULL, 10);
    const char* dsId      = PQgetisnull(aRes, r, 2) ? NULL : PQgetvalue(aRes, r, 2);
    const char* modAtIso  = PQgetisnull(aRes, r, 3) ? NULL : PQgetvalue(aRes, r, 3);
    const char* obsAtIso  = PQgetisnull(aRes, r, 4) ? NULL : PQgetvalue(aRes, r, 4);

    // The chosen time-axis ISO for range tracking.
    const char* axisIso = (strcmp(tCol, "modified_at") == 0) ? modAtIso : obsAtIso;
    if (axisIso != NULL)
    {
      if (minIso == NULL || strcmp(axisIso, minIso) < 0) minIso = axisIso;
      if (maxIso == NULL || strcmp(axisIso, maxIso) > 0) maxIso = axisIso;
    }
    const char* opStr     = PQgetvalue(aRes, r, 5);
    const char* v_text    = PQgetisnull(aRes, r, 6)  ? NULL : PQgetvalue(aRes, r, 6);
    const char* v_number  = PQgetisnull(aRes, r, 7)  ? NULL : PQgetvalue(aRes, r, 7);
    const char* v_bool    = PQgetisnull(aRes, r, 8)  ? NULL : PQgetvalue(aRes, r, 8);
    const char* v_compnd  = PQgetisnull(aRes, r, 9)  ? NULL : PQgetvalue(aRes, r, 9);
    const char* subAttrs  = PQgetisnull(aRes, r, 10) ? NULL : PQgetvalue(aRes, r, 10);
    // Column indexes 11/12 are raw timestamps (referenced only by the inner
    // window-function ORDER BY); 13 is instance_id; 14 is created_at_iso.
    const char* instId    = PQgetisnull(aRes, r, 13) ? NULL : PQgetvalue(aRes, r, 13);
    const char* crAtIso   = PQgetisnull(aRes, r, 14) ? NULL : PQgetvalue(aRes, r, 14);

    CorNode* arr = corTreeLookup(root, attrName);
    if (arr == NULL)
    {
      arr = corTreeArray(corRest.kallocP, corAllocStrdup(&corRest.kalloc, attrName));
      corTreeChildAdd(root, arr);
    }
    else if (arr->type != CorArray)
    {
      //
      // A row named as an Entity member that is no Attribute - "createdAt",
      // "modifiedAt" - found that member, a string, and an instance added to
      // a string is a dead broker. No write path records such a row now; one
      // already in a database is skipped.
      //
      COR_W("timescale: '%s' of '%s' is no Attribute - history row skipped", attrName, entityId);
      continue;
    }

    CorNode* inst = corTreeObject(corRest.kallocP, NULL);
    corTreeChildAdd(inst, corTreeString(corRest.kallocP, "type", kindToTypeString(attrKind)));

    // § 5.3.2.5: a deleted instance keeps the Attribute's type; its
    // value-field carries the NGSI-LD Null. LanguageProperty nulls take the
    // object form {"@none": "urn:ngsi-ld:null"} (§ 5.4.1).
    bool isDeleted = (strcmp(opStr, "deleted") == 0);

    if (isDeleted)
    {
      const char* vfn = kindValueFieldName(attrKind);
      if (attrKind == LdAttrLanguageProperty)
      {
        CorNode* lmP = corTreeObject(corRest.kallocP, vfn);
        corTreeChildAdd(lmP, corTreeString(corRest.kallocP, "@none", "urn:ngsi-ld:null"));
        corTreeChildAdd(inst, lmP);
      }
      else
        corTreeChildAdd(inst, corTreeString(corRest.kallocP, vfn, "urn:ngsi-ld:null"));
    }
    else
    {
      const char* vfn = kindValueFieldName(attrKind);
      CorNode* vNode = makeValueNode(corJsonP, vfn, v_text, v_number, v_bool, v_compnd);
      if (vNode != NULL)
        corTreeChildAdd(inst, vNode);
    }

    // § 6.3.11: createdAt / modifiedAt are sysAttrs and stripped by the
    // common renderHook when ?sysAttrs is not requested. We populate them
    // unconditionally so anything that runs BEFORE the strip — orderBy on
    // ?orderBy=name.createdAt, for instance — actually has values to sort
    // on. ldStripSysAttrs recurses into per-attribute instance arrays, so
    // these get cleaned up for non-sysAttr clients.
    // deletedAt is the marker that distinguishes a deletion-instance from
    // a regular one (§ 4.5.4) — it travels with the deleted row regardless
    // of sysAttrs (it's not in the strip list).
    if (crAtIso != NULL)
      corTreeChildAdd(inst, corTreeString(corRest.kallocP, "createdAt", stripZeroMs(corAllocStrdup(&corRest.kalloc, crAtIso))));
    if (modAtIso != NULL)
      corTreeChildAdd(inst, corTreeString(corRest.kallocP, "modifiedAt", stripZeroMs(corAllocStrdup(&corRest.kalloc, modAtIso))));
    if (isDeleted && modAtIso != NULL)
      corTreeChildAdd(inst, corTreeString(corRest.kallocP, "deletedAt", stripZeroMs(corAllocStrdup(&corRest.kalloc, modAtIso))));
    if (obsAtIso != NULL)
      corTreeChildAdd(inst, corTreeString(corRest.kallocP, "observedAt", stripZeroMs(corAllocStrdup(&corRest.kalloc, obsAtIso))));
    if (dsId != NULL && dsId[0] != 0)
      corTreeChildAdd(inst, corTreeString(corRest.kallocP, "datasetId", corAllocStrdup(&corRest.kalloc, dsId)));
    if (instId != NULL)
      corTreeChildAdd(inst, corTreeString(corRest.kallocP, "instanceId", corAllocStrdup(&corRest.kalloc, instId)));

    if (subAttrs != NULL && subAttrs[0] != 0)
    {
      char* dup = corAllocStrdup(&corRest.kalloc, subAttrs);
      CorNode* parsed = corJsonParse(corJsonP, dup);
      if (parsed != NULL && parsed->type == CorObject)
      {
        // corTreeChildAdd sets the added node's ->next to NULL - the next one is taken first
        CorNode* sP = parsed->value.head;
        while (sP != NULL)
        {
          CorNode* nextP = sP->next;
          corTreeChildAdd(inst, sP);
          sP = nextP;
        }
      }
    }

    corTreeChildAdd(arr, inst);
  }

  // Accumulate range info for the caller (multi-entity unions, single-entity
  // takes its values straight). Only the first writer fills size — it stays
  // constant across entities for one request.
  // minIso / maxIso point straight into aRes' libpq-owned storage, so
  // corAllocStrdup BEFORE the PQclear that frees them.
  if (rangeOut != NULL)
  {
    if (hasMore)
      rangeOut->hasMore = true;

    if (minIso != NULL)
    {
      if (rangeOut->rangeStartIso == NULL || strcmp(minIso, rangeOut->rangeStartIso) < 0)
        rangeOut->rangeStartIso = stripZeroMs(corAllocStrdup(&corRest.kalloc, minIso));
    }
    if (maxIso != NULL)
    {
      if (rangeOut->rangeEndIso == NULL || strcmp(maxIso, rangeOut->rangeEndIso) > 0)
        rangeOut->rangeEndIso = stripZeroMs(corAllocStrdup(&corRest.kalloc, maxIso));
    }

    if (rangeOut->size == 0)
      rangeOut->size = pageLimit;
  }

  PQclear(aRes);

  *treePP = root;
  return TROE_OK;
}



// -----------------------------------------------------------------------------
//
// timescaleEntityTemporalRetrieve - § 5.7.3 single-entity retrieve.
//
int timescaleEntityTemporalRetrieve(Tenant* tenantP, const char* entityId,
                                    TroeQueryFilter* fP, CorNode** resultPP,
                                    TroeRangeInfo* rangeOut)
{
  if (entityId == NULL || resultPP == NULL)
    return TROE_ERR;

  *resultPP = NULL;

  TimescaleConn* cP = timescaleConnGet(tenantP);
  if (cP == NULL) return TROE_ERR;
  timescaleConn = cP->conn;

  int r = buildEntityTemporalDocLocked(entityId, NULL, fP, resultPP, rangeOut);

  timescaleConn = NULL;
  timescaleConnRelease(cP);
  return r;
}



// -----------------------------------------------------------------------------
//
// idsInClause - " AND entity_id IN ('a','b',...)" or "" when idV is empty.
//
static const char* idsInClause(char** idV, CorAlloc* kaP)
{
  if (idV == NULL || idV[0] == NULL)
    return "";

  int needed = 32;
  for (int i = 0; idV[i] != NULL; i++)
    needed += (int) strlen(idV[i]) * 2 + 4;

  char* buf = (char*) corAlloc(kaP, needed);
  int   p   = 0;
  p += snprintf(buf + p, needed - p, " AND entity_id IN (");

  for (int i = 0; idV[i] != NULL; i++)
  {
    if (i > 0) { buf[p++] = ','; }
    buf[p++] = '\'';
    for (const char* s = idV[i]; *s; s++)
    {
      if (*s == '\'') { buf[p++] = '\''; buf[p++] = '\''; }
      else            buf[p++] = *s;
    }
    buf[p++] = '\'';
  }
  buf[p++] = ')';
  buf[p]   = 0;
  return buf;
}



// -----------------------------------------------------------------------------
//
// typesInClause - " AND entity_type && ARRAY['a','b',...]" or "" when empty.
//
// entity_type is a TEXT[] (an Entity may hold several types, § 5.2.6.4.2), so
// the filter is an array-OVERLAP test: the Entity matches when ANY of its types
// is among the requested ones. The GIN index on entity_type serves &&.
//
static const char* typesInClause(char** typeV, CorAlloc* kaP)
{
  if (typeV == NULL || typeV[0] == NULL)
    return "";

  int needed = 32;
  for (int i = 0; typeV[i] != NULL; i++)
    needed += (int) strlen(typeV[i]) * 2 + 4;

  char* buf = (char*) corAlloc(kaP, needed);
  int   p   = 0;
  p += snprintf(buf + p, needed - p, " AND entity_type && ARRAY[");

  for (int i = 0; typeV[i] != NULL; i++)
  {
    if (i > 0) { buf[p++] = ','; }
    buf[p++] = '\'';
    for (const char* s = typeV[i]; *s; s++)
    {
      if (*s == '\'') { buf[p++] = '\''; buf[p++] = '\''; }
      else            buf[p++] = *s;
    }
    buf[p++] = '\'';
  }
  buf[p++] = ']';
  buf[p]   = 0;
  return buf;
}



// -----------------------------------------------------------------------------
//
// idPatternClause - " AND entity_id ~ '<pattern>'" or "".
//
static const char* idPatternClause(const char* idPattern, CorAlloc* kaP)
{
  if (idPattern == NULL || idPattern[0] == 0)
    return "";

  int   sz  = (int) strlen(idPattern) * 2 + 32;
  char* buf = (char*) corAlloc(kaP, sz);
  int   p   = 0;
  p += snprintf(buf + p, sz - p, " AND entity_id ~ '");
  for (const char* s = idPattern; *s; s++)
  {
    if (*s == '\'') { buf[p++] = '\''; buf[p++] = '\''; }
    else            buf[p++] = *s;
  }
  buf[p++] = '\'';
  buf[p]   = 0;
  return buf;
}



// -----------------------------------------------------------------------------
//
// correlateQPred - rewrite a compiled q-predicate for use as a correlated
// subquery in the entity-selector. troeQTreeToSql emits the outer entity_id
// as the bind parameter $1 (its only parameter — value literals are inlined);
// for the set query we correlate it to the selector's row instead, i.e.
// replace every "$1" with "latest.entity_id".
//
static const char* correlateQPred(const char* qPred, CorAlloc* kaP)
{
  int   sz  = (int) strlen(qPred) + 64;   // "latest.entity_id" is longer than "$1"
  // Each "$1" (2 chars) grows to 16 chars → +14 per occurrence; bound generously.
  for (const char* s = qPred; *s != 0; s++)
    if (s[0] == '$' && s[1] == '1')
      sz += 16;

  char* buf = (char*) corAlloc(kaP, sz);
  int   p   = 0;
  for (const char* s = qPred; *s != 0; )
  {
    if (s[0] == '$' && s[1] == '1')
    {
      p += snprintf(buf + p, sz - p, "latest.entity_id");
      s += 2;
    }
    else
      buf[p++] = *s++;
  }
  buf[p] = 0;
  return buf;
}



// -----------------------------------------------------------------------------
//
// sqlQuote - append `s` to buf as an escaped single-quoted SQL literal body
// (no surrounding quotes added). Doubles embedded single quotes.
//
static int sqlQuote(char* buf, int bufSize, int p, const char* s)
{
  for (const char* c = s; *c != 0 && p < bufSize - 2; c++)
  {
    if (*c == '\'') { buf[p++] = '\''; buf[p++] = '\''; }
    else            buf[p++] = *c;
  }
  return p;
}



// -----------------------------------------------------------------------------
//
// geoRefGeometry - append the reference geometry SQL expression for the planar
// (::geometry) relations: ST_SetSRID(ST_GeomFromGeoJSON('{...}'), 4326). The
// GeoJSON is assembled from geometry-type + the raw coordinates array string,
// exactly as the generated column is built, so the comparison is like-for-like.
//
static int geoRefGeometry(char* buf, int sz, int p, const char* geometry, const char* coords)
{
  p += snprintf(buf + p, sz - p, "ST_SetSRID(ST_GeomFromGeoJSON('{\"type\":\"");
  p  = sqlQuote(buf, sz, p, geometry);
  p += snprintf(buf + p, sz - p, "\",\"coordinates\":");
  p  = sqlQuote(buf, sz, p, coords);
  p += snprintf(buf + p, sz - p, "}'), 4326)");
  return p;
}



// -----------------------------------------------------------------------------
//
// geoPredicateCorrelated - § 11.3.3 geoquery as a correlated EXISTS.
//
// Restricts to entities that have a GeoProperty instance which (a) carries the
// queried geoproperty name, (b) falls within the temporal window (the same
// timePred/op restriction the instance-match uses), and (c) satisfies the
// georel against the reference geometry. ANY in-window instance that matches
// keeps the entity (§ 11.3.3) — the natural semantics of EXISTS.
//
// Relation mapping (parity with the broker-side GEOS matcher, geoMatch.c — the
// engine the temporal store used before this pushdown; PostGIS uses GEOS for
// these predicates, so results are identical):
//   near       → ST_DWithin(geo, ref, metres)         (geography; spherical
//                metres, matching the current-state 2dsphere store). minDistance
//                → AND NOT ST_DWithin(...).
//   within     → ST_Within   (entity within reference)
//   contains   → ST_Contains (entity contains reference)
//   intersects → ST_Intersects
//   overlaps   → ST_Overlaps  (§ 7.2.4's OGC 06-103r4 overlap: equal dimension,
//                interiors meeting, neither containing the other. The mongoc
//                current-state store cannot express this and approximates it —
//                see bsonAppendGeoFilter — so it is the looser one there)
//   equals     → ST_Equals
//   disjoint   → ST_Disjoint
// The topological relations run on geo::geometry (planar) — the same plane GEOS
// works in. 2D only: Z is stored and returned (from v_compound) but ignored in
// the predicate, exactly as the current-state store does.
//
// Returns "" when there is nothing to push down.
//
static const char* geoPredicateCorrelated(TroeQueryFilter* fP, const char* tCol,
                                          const char* timePred, const char* opPred,
                                          CorAlloc* kaP)
{
  if (fP->geoRelType == LdGeoNone)
    return "";
  if (fP->geoGeometry == NULL || fP->geoCoordinates == NULL)
    return "";

  const char* geoProp = (fP->geoProperty != NULL) ? fP->geoProperty : "location";

  int   sz  = (int) strlen(geoProp) * 2
            + (int) strlen(fP->geoGeometry) * 4
            + (int) strlen(fP->geoCoordinates) * 4
            + (int) strlen(timePred) + (int) strlen(opPred) + 768;
  char* buf = (char*) corAlloc(kaP, sz);
  int   p   = 0;

  p += snprintf(buf + p, sz - p,
    " AND EXISTS (SELECT 1 FROM troe_attrs WHERE entity_id = latest.entity_id"
    " AND attr_name = '");
  p  = sqlQuote(buf, sz, p, geoProp);
  p += snprintf(buf + p, sz - p, "' AND attr_kind = 3 AND geo IS NOT NULL%s%s", timePred, opPred);

  switch (fP->geoRelType)
  {
  case LdGeoNear:
    // geography column, spherical metres — matches the current-state 2dsphere.
    p += snprintf(buf + p, sz - p, " AND ST_DWithin(geo, geography(");
    p  = geoRefGeometry(buf, sz, p, fP->geoGeometry, fP->geoCoordinates);
    p += snprintf(buf + p, sz - p, "), %f)", fP->geoMaxDistance);
    if (fP->geoMinDistance >= 0)
    {
      p += snprintf(buf + p, sz - p, " AND NOT ST_DWithin(geo, geography(");
      p  = geoRefGeometry(buf, sz, p, fP->geoGeometry, fP->geoCoordinates);
      p += snprintf(buf + p, sz - p, "), %f)", fP->geoMinDistance);
    }
    break;

  case LdGeoWithin:
  case LdGeoContains:
  case LdGeoIntersects:
  case LdGeoOverlaps:
  case LdGeoEquals:
  case LdGeoDisjoint:
  {
    const char* fn = "ST_Intersects";
    switch (fP->geoRelType)
    {
      case LdGeoWithin:     fn = "ST_Within";     break;
      case LdGeoContains:   fn = "ST_Contains";   break;
      case LdGeoIntersects: fn = "ST_Intersects"; break;
      case LdGeoOverlaps:   fn = "ST_Overlaps";   break;
      case LdGeoEquals:     fn = "ST_Equals";     break;
      case LdGeoDisjoint:   fn = "ST_Disjoint";   break;
      default:                                    break;
    }
    p += snprintf(buf + p, sz - p, " AND %s(geo::geometry, ", fn);
    p  = geoRefGeometry(buf, sz, p, fP->geoGeometry, fP->geoCoordinates);
    p += snprintf(buf + p, sz - p, ")");
    break;
  }

  default:
    break;
  }

  p += snprintf(buf + p, sz - p, ")");
  buf[p] = 0;
  return buf;
}



// -----------------------------------------------------------------------------
//
// AGGR_DECLINED - aggregatedDocsLocked cannot reproduce this aggregation
// exactly; the caller builds the raw-instance docs and the renderHook
// aggregates them, as for any query without aggrPushdown.
//
#define AGGR_DECLINED  2



// -----------------------------------------------------------------------------
//
// aggrNsToIso - epoch-ns to ISO 8601 (UTC), `.SSS` only when the milliseconds
// are non-zero.
//
// The bucket bounds must render exactly as ldToAggregatedValues renders them -
// the two paths answer the same query, depending only on where it was
// aggregated.
//
static char* aggrNsToIso(uint64_t ns, CorAlloc* kaP)
{
  char*     buf = (char*) corAlloc(kaP, 64);
  time_t    t   = (time_t) (ns / 1000000000ULL);
  long      ms  = (long) ((ns % 1000000000ULL) / 1000000);
  struct tm tmv;

  gmtime_r(&t, &tmv);

  if (ms == 0)
    snprintf(buf, 64, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  else
    snprintf(buf, 64, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);

  return buf;
}



// -----------------------------------------------------------------------------
//
// aggrPushdownOk - can this query be aggregated in SQL, with the very answer
// the renderHook would give?
//
// ldToAggregatedValues buckets the rendered observedAt/modifiedAt, which carry
// milliseconds (created_at/modified_at microseconds, truncated to ms when
// rendered as observedAt). SQL sees the microseconds. The two agree on every
// bucket as long as each bucket boundary sits on a whole millisecond - hence
// the ms alignment of timeAt and of the period. Calendar periods (months,
// years) have no constant width, so they stay with the renderHook, as do the
// createdAt/deletedAt axes (not the column the renderHook reads) and temporal
// pagination (which aggregates one page, not the window).
//
static bool aggrPushdownOk(TroeQueryFilter* fP)
{
  if (!fP->aggrPushdown || (fP->aggrMethodsV == NULL))                      return false;
  if ((fP->timerel == NULL) || (strcmp(fP->timerel, "between") != 0))       return false;
  if ((fP->timeAtNs == 0) || (fP->endTimeAtNs <= fP->timeAtNs))             return false;
  if ((fP->timeAtNs % 1000000) != 0)                                        return false;
  if ((fP->aggrPeriodMonths != 0) || ((fP->aggrPeriodNs % 1000000) != 0))   return false;
  if ((fP->lastN != 0) || (fP->firstN != 0) || (fP->offsetN != 0))          return false;

  const char* tp = fP->timeproperty;
  if ((tp != NULL) && (strcmp(tp, "observedAt") != 0) && (strcmp(tp, "modifiedAt") != 0))
    return false;

  return true;
}



// -----------------------------------------------------------------------------
//
// aggrPageIndex - index of entityId in the page (PQ result column 0), -1 if absent.
//
// Every result walked here is ORDER BY entity_id like the page, so the next
// match is almost always at or right after *hintP - which makes the walk
// linear. The wrap-around only guards against the two orders disagreeing.
//
static int aggrPageIndex(PGresult* pageRes, int pageN, const char* entityId, int* hintP)
{
  for (int n = 0; n < pageN; n++)
  {
    int i = (*hintP + n) % pageN;

    if (strcmp(PQgetvalue(pageRes, i, 0), entityId) == 0)
    {
      *hintP = i;
      return i;
    }
  }

  return -1;
}



// -----------------------------------------------------------------------------
//
// aggrTuple - [value, bucket-start, bucket-end]
//
static CorNode* aggrTuple(CorAlloc* allocP, double v, uint64_t startNs, uint64_t endNs)
{
  CorNode* tupleP = corTreeArray(allocP, NULL);

  corTreeChildAdd(tupleP, corTreeFloat(allocP, NULL, v));
  corTreeChildAdd(tupleP, corTreeString(allocP, NULL, aggrNsToIso(startNs, &corRest.kalloc)));
  corTreeChildAdd(tupleP, corTreeString(allocP, NULL, aggrNsToIso(endNs, &corRest.kalloc)));

  return tupleP;
}



// -----------------------------------------------------------------------------
//
// aggrAttribute - the aggregated Attribute from its bucket rows [r0, r1) of sRes.
//
// Same methods, same formulas and the same empty-bucket rules as
// ldToAggregatedValues::emitValueArray for a numeric Property. Returned inside a
// dataset-keyed wrapper ({"@none": {...}}), the storage shape ldEntityToApi
// unwraps back into a plain Attribute; ldToAggregatedValues skips it, as it is
// no instance array.
//
// sRes columns: 0 entity_id, 1 attr_name, 2 bucket start (epoch us), 3 total, 4 distinct, 5 sum, 6 sumsq, 7 min, 8 max
//
static CorNode* aggrAttribute(PGresult* sRes, int r0, int r1, TroeQueryFilter* fP, CorAlloc* allocP)
{
  CorNode* wrapperP = corTreeObject(allocP, corAllocStrdup(&corRest.kalloc, PQgetvalue(sRes, r0, 1)));
  CorNode* attrP   = corTreeObject(allocP, "@none");

  corTreeChildAdd(attrP, corTreeString(allocP, "type", "Property"));

  bool     zeroPeriod = (fP->aggrPeriodNs == 0);
  uint64_t endNs      = fP->endTimeAtNs;

  for (int m = 0; fP->aggrMethodsV[m] != NULL; m++)
  {
    const char* method = fP->aggrMethodsV[m];
    CorNode*    arrP   = corTreeArray(allocP, method);

    for (int r = r0; r < r1; r++)
    {
      uint64_t  bStart  = (uint64_t) strtoll(PQgetvalue(sRes, r, 2), NULL, 10) * 1000;
      double    n       = strtod(PQgetvalue(sRes, r, 3), NULL);
      double    sum     = strtod(PQgetvalue(sRes, r, 5), NULL);
      double    sumsq   = strtod(PQgetvalue(sRes, r, 6), NULL);
      uint64_t  bEnd    = zeroPeriod ? endNs : bStart + fP->aggrPeriodNs;
      double    v;

      if (bEnd > endNs)   // clipped to the explicit endTimeAt
        bEnd = endNs;

      if      ((strcmp(method, "totalCount") == 0) || (strcmp(method, "count") == 0))  v = n;
      else if (strcmp(method, "distinctCount") == 0)                                  v = strtod(PQgetvalue(sRes, r, 4), NULL);
      else if (strcmp(method, "sum")           == 0)                                  v = sum;
      else if (strcmp(method, "sumsq")         == 0)                                  v = sumsq;
      else if (strcmp(method, "avg")           == 0)                                  v = sum / n;
      else if (strcmp(method, "min")           == 0)                                  v = strtod(PQgetvalue(sRes, r, 7), NULL);
      else if (strcmp(method, "max")           == 0)                                  v = strtod(PQgetvalue(sRes, r, 8), NULL);
      else if (strcmp(method, "stddev")        == 0)
      {
        if (n < 2)
          continue;

        double mean = sum / n;
        double var  = (sumsq / n) - (mean * mean);

        v = (var > 0.0) ? sqrt(var) : 0.0;
      }
      else
        continue;

      corTreeChildAdd(arrP, aggrTuple(allocP, v, bStart, bEnd));
    }

    corTreeChildAdd(attrP, arrP);
  }

  corTreeChildAdd(wrapperP, attrP);
  return wrapperP;
}



// -----------------------------------------------------------------------------
//
// aggregatedDocsLocked - the page's EntityTemporal docs, already aggregated.
//
// Three statements for the whole page, where the raw-instance path runs three
// PER ENTITY and ships every instance to the broker: the Entities' system
// timestamps, and one GROUP BY (entity, attribute, bucket) that leaves only the
// accumulators - count, distinct count, sum, sum of squares, min, max - to cross
// the wire. The methods are computed from those exactly as ldToAggregatedValues
// computes them.
//
// Returns AGGR_DECLINED when any instance in the window is something other than
// a Number or Boolean Property value (strings, compounds, Relationships,
// deletions, ...) or when an attribute holds more instances than the instance
// cap - that path aggregates only the capped page. The caller then takes the
// raw-instance path for the whole request.
//
static int aggregatedDocsLocked(PGresult*        pageRes,
                                int              pageN,
                                TroeQueryFilter* fP,
                                const char*      whereTail,
                                int              nParams,
                                const char**     paramV,
                                CorNode*         arrP,
                                TroeRangeInfo*   rangeOut)
{
  if (pageN == 0)
    return TROE_OK;

  char** idV = (char**) corAlloc(&corRest.kalloc, (pageN + 1) * sizeof(char*));
  for (int i = 0; i < pageN; i++)
    idV[i] = PQgetvalue(pageRes, i, 0);
  idV[pageN] = NULL;

  const char* idPred      = idsInClause(idV, &corRest.kalloc);
  const char* tCol        = timeColumn(fP->timeproperty);
  int         instanceCap = (fP->instanceCap > 0) ? fP->instanceCap : (timescaleInstanceCap > 0 ? timescaleInstanceCap : TROE_DEFAULT_INSTANCE_CAP);

  //
  // The bucket, as its start: date_bin with timeAt as origin is exactly the
  // renderHook's timeAt + k * period. Plain postgres (14+), as this plugin also
  // runs without the TimescaleDB extension - time_bucket is the same thing,
  // only not always there. Zero period: one bucket, starting at timeAt.
  //
  char bucketExpr[160];
  if (fP->aggrPeriodNs == 0)
    snprintf(bucketExpr, sizeof(bucketExpr), "$1::timestamptz");
  else
    snprintf(bucketExpr, sizeof(bucketExpr), "date_bin('%llu microseconds'::interval, %s, $1::timestamptz)",
             (unsigned long long) (fP->aggrPeriodNs / 1000), tCol);

  //
  // COUNT(DISTINCT) only when asked for: it sorts every instance and keeps the
  // query from running in parallel - five times the cost of all the rest.
  //
  bool distinct = false;
  for (int m = 0; fP->aggrMethodsV[m] != NULL; m++)
  {
    if (strcmp(fP->aggrMethodsV[m], "distinctCount") == 0)
      distinct = true;
  }

  //
  // num: the value as the renderHook's numeric accumulator sees it - a Boolean
  // counts as 1 / 0 (§ 4.5.19.1). ok: the instance is one of those two kinds.
  // The distinct count is capped at 64, the renderHook's distinct-set size.
  //
  // SUM adds in whatever order the scan (or the parallel workers) deliver, the
  // renderHook in time order - floating point, so a sum can differ in its last
  // bit. Forcing the order (SUM(num ORDER BY ...)) would forbid the parallel
  // plan, which is most of the gain.
  //
  int   sSize = (int) strlen(idPred) + (int) strlen(whereTail) + 2048;
  char* sSql  = (char*) corAlloc(&corRest.kalloc, sSize);

  snprintf(sSql, sSize,
    "SELECT entity_id, attr_name, (EXTRACT(EPOCH FROM bucket) * 1000000)::bigint, COUNT(*), %s, SUM(num), SUM(num * num), MIN(num), MAX(num), BOOL_AND(ok) "
    "FROM (SELECT entity_id, attr_name, %s AS bucket, "
    "             COALESCE(v_number, CASE WHEN v_bool THEN 1.0::float8 WHEN NOT v_bool THEN 0.0::float8 END) AS num, "
    "             (attr_kind = %d AND op <> 'deleted' AND v_compound IS NULL AND (v_number IS NOT NULL OR v_bool IS NOT NULL)) AS ok "
    "      FROM troe_attrs WHERE TRUE%s%s) s "
    "GROUP BY entity_id, attr_name, bucket "
    "ORDER BY entity_id, attr_name, bucket",
    distinct ? "LEAST(COUNT(DISTINCT num), 64)" : "0", bucketExpr, LdAttrProperty, idPred, whereTail);

  PGresult* sRes = PQexecParams(timescaleConn, sSql, nParams, NULL, (nParams > 0) ? paramV : NULL, NULL, NULL, 0);
  if (PQresultStatus(sRes) != PGRES_TUPLES_OK)
  {
    COR_E("timescale: aggregation SELECT failed: %s", PQerrorMessage(timescaleConn));
    PQclear(sRes);
    return TROE_ERR;
  }

  int rowN = PQntuples(sRes);

  // Anything the renderHook would aggregate differently: decline, before any doc is built
  {
    long long   attrTotal = 0;
    const char* prevE     = NULL;
    const char* prevA     = NULL;

    for (int r = 0; r < rowN; r++)
    {
      if (PQgetvalue(sRes, r, 9)[0] != 't')
      {
        PQclear(sRes);
        return AGGR_DECLINED;
      }

      const char* e = PQgetvalue(sRes, r, 0);
      const char* a = PQgetvalue(sRes, r, 1);

      if ((prevE == NULL) || (strcmp(e, prevE) != 0) || (strcmp(a, prevA) != 0))
        attrTotal = 0;

      attrTotal += strtoll(PQgetvalue(sRes, r, 3), NULL, 10);
      if (attrTotal > instanceCap)
      {
        PQclear(sRes);
        return AGGR_DECLINED;
      }

      prevE = e;
      prevA = a;
    }
  }

  // The Entities' system timestamps - see buildEntityTemporalDocLocked
  int   tSize = (int) strlen(idPred) + 512;
  char* tSql  = (char*) corAlloc(&corRest.kalloc, tSize);

  snprintf(tSql, tSize,
    "SELECT entity_id, "
    "       to_char(MIN(modified_at) FILTER (WHERE op = 'created') AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), "
    "       to_char(MAX(modified_at) AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), "
    "       (ARRAY_AGG(op ORDER BY modified_at DESC))[1] "
    "FROM troe_entities WHERE TRUE%s GROUP BY entity_id ORDER BY entity_id",
    idPred);

  PGresult* tRes = PQexecParams(timescaleConn, tSql, 0, NULL, NULL, NULL, NULL, 0);
  if (PQresultStatus(tRes) != PGRES_TUPLES_OK)
  {
    COR_E("timescale: entity-timestamps SELECT failed: %s", PQerrorMessage(timescaleConn));
    PQclear(tRes);
    PQclear(sRes);
    return TROE_ERR;
  }

  CorAlloc* allocP = corRest.kallocP;
  CorNode** docV   = (CorNode**) corAlloc(&corRest.kalloc, pageN * sizeof(CorNode*));
  int       hint   = 0;

  for (int i = 0; i < pageN; i++)
  {
    CorNode* docP = corTreeObject(allocP, NULL);

    corTreeChildAdd(docP, corTreeString(allocP, "id", corAllocStrdup(&corRest.kalloc, PQgetvalue(pageRes, i, 0))));

    if (!PQgetisnull(pageRes, i, 1))
    {
      CorNode* typeNodeP = typeNodeFromJson(corAllocStrdup(&corRest.kalloc, PQgetvalue(pageRes, i, 1)), corRest.corJsonP, &corRest.kalloc);
      if (typeNodeP != NULL)
        corTreeChildAdd(docP, typeNodeP);
    }

    docV[i] = docP;
  }

  for (int r = 0; r < PQntuples(tRes); r++)
  {
    int i = aggrPageIndex(pageRes, pageN, PQgetvalue(tRes, r, 0), &hint);
    if (i < 0)
      continue;

    if (!PQgetisnull(tRes, r, 1))
      corTreeChildAdd(docV[i], corTreeString(allocP, "createdAt", stripZeroMs(corAllocStrdup(&corRest.kalloc, PQgetvalue(tRes, r, 1)))));
    if (!PQgetisnull(tRes, r, 2))
      corTreeChildAdd(docV[i], corTreeString(allocP, "modifiedAt", stripZeroMs(corAllocStrdup(&corRest.kalloc, PQgetvalue(tRes, r, 2)))));
    if (!PQgetisnull(tRes, r, 2) && !PQgetisnull(tRes, r, 3) && (strcmp(PQgetvalue(tRes, r, 3), "deleted") == 0))
      corTreeChildAdd(docV[i], corTreeString(allocP, "deletedAt", stripZeroMs(corAllocStrdup(&corRest.kalloc, PQgetvalue(tRes, r, 2)))));
  }
  PQclear(tRes);

  // One Attribute per (entity, attr_name) run of bucket rows
  bool* hasAttrsV = (bool*) corAlloc(&corRest.kalloc, pageN * sizeof(bool));
  memset(hasAttrsV, 0, pageN * sizeof(bool));

  hint = 0;
  for (int r0 = 0; r0 < rowN; )
  {
    const char* e  = PQgetvalue(sRes, r0, 0);
    const char* a  = PQgetvalue(sRes, r0, 1);
    int         r1 = r0 + 1;

    while ((r1 < rowN) && (strcmp(PQgetvalue(sRes, r1, 0), e) == 0) && (strcmp(PQgetvalue(sRes, r1, 1), a) == 0))
      r1++;

    int i = aggrPageIndex(pageRes, pageN, e, &hint);
    if (i >= 0)
    {
      corTreeChildAdd(docV[i], aggrAttribute(sRes, r0, r1, fP, allocP));
      hasAttrsV[i] = true;
    }

    r0 = r1;
  }
  PQclear(sRes);

  // Page order; an Entity without an Attribute in the window is left out, as in the raw path
  for (int i = 0; i < pageN; i++)
  {
    if (hasAttrsV[i])
      corTreeChildAdd(arrP, docV[i]);
  }

  if (rangeOut->size == 0)
    rangeOut->size = instanceCap;

  return TROE_OK;
}



// -----------------------------------------------------------------------------
//
// timescaleEntityTemporalQuery - § 5.7.4 multi-entity query.
//
// One set query selects exactly the matching entities — entity-level
// selectors (id / idPattern / type), a correlated EXISTS that the entity
// has at least one instance inside the time window (timerel / attrs /
// datasetId / timeproperty), and the correlated q predicate — paginated
// server-side (ORDER BY entity_id, LIMIT/OFFSET). Only the page's entities
// have their EntityTemporal doc built. ?count adds a COUNT(*) over the same
// WHERE for NGSILD-Results-Count.
//
int timescaleEntityTemporalQuery(Tenant* tenantP, TroeQueryFilter* fP,
                                 CorNode** resultPP, TroeRangeInfo* rangeOut)
{
  if (fP == NULL || resultPP == NULL)
    return TROE_ERR;

  *resultPP = NULL;
  rangeOut->entityCount = -1;     // not computed unless ?count

  TimescaleConn* cP = timescaleConnGet(tenantP);
  if (cP == NULL) return TROE_ERR;
  timescaleConn = cP->conn;

  CorAlloc* allocP = corRest.kallocP;
  CorNode* arrP  = corTreeArray(allocP, NULL);

  // limitGiven distinguishes an explicit limit=0 (count-only page) from an
  // absent limit (broker default). limit=0 → fetch LIMIT 1 (to set
  // moreEntities) but build 0 docs; the COUNT(*) still runs under ?count.
  int limit  = fP->limitGiven ? fP->limit : 1000;
  int offset = (fP->offset > 0) ? fP->offset : 0;

  // Entity-level selectors.
  const char* idClause      = idsInClause(fP->idV, &corRest.kalloc);
  const char* typeClause    = typesInClause(fP->typeV, &corRest.kalloc);
  const char* patternClause = idPatternClause(fP->idPattern, &corRest.kalloc);

  // Instance-match predicate (mirrors buildEntityTemporalDocLocked's attr
  // query): timerel window on the timeproperty column, attrs/datasetId
  // IN-clauses, and the createdAt/deletedAt op restriction. timeAt/endTimeAt
  // bind to the selector's $1/$2.
  const char* timeProp = fP->timeproperty;
  const char* tCol     = timeColumn(timeProp);
  const char* opPred   = "";
  if (timeProp != NULL && strcmp(timeProp, "createdAt") == 0)
    opPred = " AND op = 'created'";
  else if (timeProp != NULL && strcmp(timeProp, "deletedAt") == 0)
    opPred = " AND op = 'deleted'";
  const char* attrPred = attrsInClause(fP->attrV, &corRest.kalloc);
  const char* dsPred   = datasetIdsInClause(fP->datasetIdV, &corRest.kalloc);

  const char* timePred = "";
  int         nParams  = 0;
  const char* paramV[2];
  if (fP->timerel != NULL)
  {
    if (strcmp(fP->timerel, "before") == 0)
    {
      char* b = (char*) corAlloc(&corRest.kalloc, 64);
      snprintf(b, 64, " AND %s < $1::timestamptz", tCol);
      timePred = b; paramV[0] = fP->timeAtIso; nParams = 1;
    }
    else if (strcmp(fP->timerel, "after") == 0)
    {
      char* b = (char*) corAlloc(&corRest.kalloc, 64);
      snprintf(b, 64, " AND %s >= $1::timestamptz", tCol);
      timePred = b; paramV[0] = fP->timeAtIso; nParams = 1;
    }
    else if (strcmp(fP->timerel, "between") == 0)
    {
      char* b = (char*) corAlloc(&corRest.kalloc, 96);
      snprintf(b, 96, " AND %s >= $1::timestamptz AND %s < $2::timestamptz", tCol, tCol);
      timePred = b; paramV[0] = fP->timeAtIso; paramV[1] = fP->endTimeAtIso; nParams = 2;
    }
  }

  // Correlated q predicate (entity-level precondition).
  const char* qCorr = "";
  if (fP->qSqlPredicate != NULL)
  {
    const char* c  = correlateQPred(fP->qSqlPredicate, &corRest.kalloc);
    int         sz = (int) strlen(c) + 8;
    char*       b  = (char*) corAlloc(&corRest.kalloc, sz);
    snprintf(b, sz, " AND %s", c);
    qCorr = b;
  }

  // Correlated geo predicate (§ 11.3.3) — pushes ?georel near into SQL.
  const char* geoPred = geoPredicateCorrelated(fP, tCol, timePred, opPred, &corRest.kalloc);

  // WHERE body shared by the page query and the count query.
  int   wSize = 16384;
  char* where = (char*) corAlloc(&corRest.kalloc, wSize);
  snprintf(where, wSize,
    "FROM (SELECT DISTINCT ON (entity_id) entity_id, entity_type, modified_at "
    "FROM troe_entities ORDER BY entity_id, modified_at DESC) latest "
    "WHERE TRUE%s%s%s "
    "AND EXISTS (SELECT 1 FROM troe_attrs WHERE entity_id = latest.entity_id%s%s%s%s)%s%s",
    idClause, typeClause, patternClause, timePred, opPred, attrPred, dsPred, qCorr, geoPred);

  // Page query — fetch limit+1 so we can tell whether more entities remain.
  int   pSize = wSize + 256;
  char* pageSql = (char*) corAlloc(&corRest.kalloc, pSize);
  snprintf(pageSql, pSize,
    "SELECT entity_id, array_to_json(entity_type)::text %s ORDER BY entity_id LIMIT %d OFFSET %d",
    where, limit + 1, offset);

  PGresult* eRes = PQexecParams(timescaleConn, pageSql, nParams, NULL,
                                (nParams > 0) ? paramV : NULL, NULL, NULL, 0);
  if (PQresultStatus(eRes) != PGRES_TUPLES_OK)
  {
    COR_E("timescale: entity-selector SELECT failed: %s", PQerrorMessage(timescaleConn));
    PQclear(eRes);
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_ERR;
  }

  int rowN  = PQntuples(eRes);
  int pageN = (rowN > limit) ? limit : rowN;     // the extra row only signals "more"
  rangeOut->moreEntities = (rowN > limit);

  // § 4.5.20 aggregatedValues aggregated here, in SQL - else the raw instances, one Entity at a time
  int aggrRc = AGGR_DECLINED;

  if (aggrPushdownOk(fP))
  {
    int   tailSize = (int) (strlen(timePred) + strlen(opPred) + strlen(attrPred) + strlen(dsPred)) + 1;
    char* tail     = (char*) corAlloc(&corRest.kalloc, tailSize);

    snprintf(tail, tailSize, "%s%s%s%s", timePred, opPred, attrPred, dsPred);

    aggrRc = aggregatedDocsLocked(eRes, pageN, fP, tail, nParams, paramV, arrP, rangeOut);
    if (aggrRc == TROE_ERR)
    {
      PQclear(eRes);
      timescaleConn = NULL;
      timescaleConnRelease(cP);
      return TROE_ERR;
    }
  }

  for (int r = 0; (aggrRc == AGGR_DECLINED) && (r < pageN); r++)
  {
    const char* entityId   = corAllocStrdup(&corRest.kalloc, PQgetvalue(eRes, r, 0));
    const char* entityType = PQgetisnull(eRes, r, 1) ? NULL : corAllocStrdup(&corRest.kalloc, PQgetvalue(eRes, r, 1));

    CorNode* docP = NULL;
    int rc = buildEntityTemporalDocLocked(entityId, entityType, fP, &docP, rangeOut);

    if (rc == TROE_ERR)
    {
      PQclear(eRes);
      timescaleConn = NULL;
      timescaleConnRelease(cP);
      return TROE_ERR;
    }
    if (rc != TROE_OK || docP == NULL)
      continue;

    // The selector already guarantees matching content; this guards only the
    // pathological case where a large per-attribute offsetN pages out every
    // instance, leaving an empty doc.
    bool hasAttrs = false;
    for (CorNode* c = docP->value.head; c != NULL; c = c->next)
    {
      if (c->name == NULL)                         continue;
      if (strcmp(c->name, "id")         == 0)      continue;
      if (strcmp(c->name, "type")       == 0)      continue;
      if (strcmp(c->name, "scope")      == 0)      continue;
      if (strcmp(c->name, "createdAt")  == 0)      continue;
      if (strcmp(c->name, "modifiedAt") == 0)      continue;
      hasAttrs = true;
      break;
    }
    if (!hasAttrs)
      continue;

    corTreeChildAdd(arrP, docP);
  }

  PQclear(eRes);

  // § 6.4.6 count — total matching entities over the same WHERE (no LIMIT).
  if (fP->count)
  {
    int   cSize = wSize + 64;
    char* countSql = (char*) corAlloc(&corRest.kalloc, cSize);
    snprintf(countSql, cSize, "SELECT COUNT(*) %s", where);

    PGresult* nRes = PQexecParams(timescaleConn, countSql, nParams, NULL,
                                  (nParams > 0) ? paramV : NULL, NULL, NULL, 0);
    if (PQresultStatus(nRes) == PGRES_TUPLES_OK && PQntuples(nRes) > 0)
      rangeOut->entityCount = strtol(PQgetvalue(nRes, 0, 0), NULL, 10);
    else
      COR_E("timescale: entity-count SELECT failed: %s", PQerrorMessage(timescaleConn));
    PQclear(nRes);
  }

  timescaleConn = NULL;
  timescaleConnRelease(cP);

  *resultPP = arrP;
  return TROE_OK;
}
