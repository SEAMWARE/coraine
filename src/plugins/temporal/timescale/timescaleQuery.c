//
// FILE            timescaleQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
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
#include <string.h>                                       // strcmp
#include <stdlib.h>                                       // strtol, strtod
#include <pthread.h>                                      // pthread_mutex_lock
#include <libpq-fe.h>                                     // PG*

#include "ktrace/kTrace.h"                                // KT_E
#include "kjson/kjBuilder.h"                              // kjObject, kjArray, kjString, kjInteger, kjFloat, kjBoolean, kjChildAdd
#include "kjson/kjLookup.h"                               // kjLookup
#include "kjson/kjParse.h"                                // kjParse
#include "kalloc/kaAlloc.h"                               // kaAlloc
#include "kalloc/kaStrdup.h"                              // kaStrdup

#include "swRest/SwRestState.h"                           // swRest
#include "swNgsild/LdAttrType.h"                          // LdAttr*
#include "swNgsild/SwNgsild.h"                            // swNgsild

#include "troe/TroeDriver.h"                              // TroeQueryFilter, TROE_*

#include "temporal/timescale/timescaleGlobals.h"          // timescaleConn, timescaleMutex
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
static KjNode* makeValueNode(Kjson* kjsonP, const char* vfn,
                             const char* v_text, const char* v_number,
                             const char* v_bool, const char* v_compnd)
{
  if (v_compnd != NULL && v_compnd[0] != 0)
  {
    char*   dup    = kaStrdup(&swRest.kalloc, v_compnd);
    KjNode* parsed = kjParse(kjsonP, dup);
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
      return kjInteger(kjsonP, vfn, (long long) n);
    return kjFloat(kjsonP, vfn, n);
  }
  if (v_bool != NULL)
    return kjBoolean(kjsonP, vfn, (v_bool[0] == 't') ? KTRUE : KFALSE);
  if (v_text != NULL)
    return kjString(kjsonP, vfn, kaStrdup(&swRest.kalloc, v_text));
  return NULL;
}



// -----------------------------------------------------------------------------
//
// stripZeroMs - trim trailing ".000Z" → "Z" so a clean second renders
// without the artificial sub-second padding (matches the canonical
// fixtures used by ETSI's temporal tests). The buffer is in-place
// rewritable: postgres' to_char output lives in PQgetvalue's libpq-
// owned storage, so we kaStrdup first, then trim. Caller passes in the
// strdup'd copy.
//
static char* stripZeroMs(char* s)
{
  if (s == NULL) return NULL;
  size_t n = strlen(s);
  if (n >= 5 && memcmp(s + n - 5, ".000Z", 5) == 0)
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
static const char* attrsInClause(char** attrV, KAlloc* kaP)
{
  if (attrV == NULL || attrV[0] == NULL)
    return "";

  int needed = 32;
  for (int i = 0; attrV[i] != NULL; i++)
    needed += (int) strlen(attrV[i]) * 2 + 4;

  char* buf = (char*) kaAlloc(kaP, needed);
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
static const char* datasetIdsInClause(char** dsV, KAlloc* kaP)
{
  if (dsV == NULL || dsV[0] == NULL)
    return "";

  int needed = 32;
  for (int i = 0; dsV[i] != NULL; i++)
    needed += (int) strlen(dsV[i]) * 2 + 4;

  char* buf = (char*) kaAlloc(kaP, needed);
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
// Mutex must be held by the caller.
//
static bool runQPreconditionLocked(const char* qPred, const char* entityId,
                                   const char* tenant, bool* errOut)
{
  if (qPred == NULL)
    return true;

  // troeQTreeToSql emits $1 for entity_id and $2 for tenant inside every
  // EXISTS-subquery, so the caller binds those two params here.
  const char* idParam[2] = { entityId, tenant };
  int   sz  = (int) strlen(qPred) + 32;
  char* sql = (char*) kaAlloc(&swRest.kalloc, sz);
  snprintf(sql, sz, "SELECT %s", qPred);

  PGresult* res = PQexecParams(timescaleConn, sql, 2, NULL, idParam, NULL, NULL, 0);
  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    KT_E("timescale: q precondition SELECT failed: %s", PQerrorMessage(timescaleConn));
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
// buildEntityTemporalDocLocked - build the EntityTemporal tree for one entity.
//
// Mutex must be held. Returns:
//   TROE_OK         — *treePP set to the built tree
//   TROE_NOT_FOUND  — entity has no temporal data (or q didn't match)
//   TROE_ERR        — DB error
//
// entityType is optional; when NULL, the helper looks it up via troe_entities.
//
static int buildEntityTemporalDocLocked(const char* tenant, const char* entityId,
                                        const char* entityTypeIn,
                                        TroeQueryFilter* fP, KjNode** treePP,
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
  const char* attrPred      = attrsInClause(attrV, &swRest.kalloc);
  const char* dsPred        = datasetIdsInClause(datasetIdV, &swRest.kalloc);

  // q precondition (tenant-scoped — the q-tree compiler emits AND tenant=$2).
  bool qErr = false;
  if (!runQPreconditionLocked(qPred, entityId, tenant, &qErr))
    return qErr ? TROE_ERR : TROE_NOT_FOUND;

  // Entity type — caller may pass it in (multi-entity case fetches in one
  // go); single-entity look-up here.
  const char* entityType = entityTypeIn;
  if (entityType == NULL)
  {
    const char* idParam[2] = { entityId, tenant };
    PGresult* eRes = PQexecParams(timescaleConn,
      "SELECT entity_type FROM troe_entities "
      "WHERE entity_id = $1 AND tenant = $2 "
      "ORDER BY modified_at DESC LIMIT 1",
      2, NULL, idParam, NULL, NULL, 0);
    if (PQresultStatus(eRes) != PGRES_TUPLES_OK)
    {
      KT_E("timescale: troe_entities SELECT failed: %s", PQerrorMessage(timescaleConn));
      PQclear(eRes);
      return TROE_ERR;
    }
    if (PQntuples(eRes) > 0)
      entityType = kaStrdup(&swRest.kalloc, PQgetvalue(eRes, 0, 0));
    PQclear(eRes);
  }

  // Attribute query.
  // $1 = entity_id, $2 = tenant. timeAt/endTimeAt occupy $3 / $4 when set.
  const char* timePred = "";
  int         nParams  = 2;
  const char* paramV[4];
  paramV[0] = entityId;
  paramV[1] = tenant;

  if (timerel != NULL)
  {
    // § 4.11.4 timerel bounds:
    //   before  — timeAt is EXCLUSIVE (strict less-than)
    //   after   — timeAt is INCLUSIVE (≥)
    //   between — [timeAt, endTimeAt): lower inclusive, upper exclusive
    if (strcmp(timerel, "before") == 0)
    {
      char* buf = (char*) kaAlloc(&swRest.kalloc, 64);
      snprintf(buf, 64, " AND %s < $3::timestamptz", tCol);
      timePred = buf;
      paramV[2] = timeAt;
      nParams   = 3;
    }
    else if (strcmp(timerel, "after") == 0)
    {
      char* buf = (char*) kaAlloc(&swRest.kalloc, 64);
      snprintf(buf, 64, " AND %s >= $3::timestamptz", tCol);
      timePred = buf;
      paramV[2] = timeAt;
      nParams   = 3;
    }
    else if (strcmp(timerel, "between") == 0)
    {
      char* buf = (char*) kaAlloc(&swRest.kalloc, 96);
      snprintf(buf, 96, " AND %s >= $3::timestamptz AND %s < $4::timestamptz", tCol, tCol);
      timePred = buf;
      paramV[2] = timeAt;
      paramV[3] = endTimeAt;
      nParams   = 4;
    }
  }

  const char* selectCols =
    "attr_name, attr_kind, dataset_id, "
    "to_char(modified_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS modified_at_iso, "
    "to_char(observed_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS observed_at_iso, "
    "op, v_text, v_number, v_bool, v_compound, sub_attrs, "
    "modified_at, observed_at, instance_id, "
    "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS created_at_iso";

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
    "FROM troe_attrs WHERE entity_id = $1 AND tenant = $2%s%s%s%s "
    "GROUP BY attr_name, dataset_id",
    timePred, opPred, attrPred, dsPred);

  PGresult* gRes = PQexecParams(timescaleConn, groupSql, nParams, NULL, paramV, NULL, NULL, 0);
  if (PQresultStatus(gRes) != PGRES_TUPLES_OK)
  {
    KT_E("timescale: troe_attrs group-pre-pass failed: %s", PQerrorMessage(timescaleConn));
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
  char* sql     = (char*) kaAlloc(&swRest.kalloc, sqlSize);

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
    "WHERE entity_id = $1 AND tenant = $2%s%s%s%s) sub "
    "WHERE %s "
    "ORDER BY attr_name, dataset_id, %s %s",
    selectCols, tCol, orderDir, timePred, opPred, attrPred, dsPred, rnClip, tCol, orderDir);

  PGresult* aRes = PQexecParams(timescaleConn, sql, nParams, NULL, paramV, NULL, NULL, 0);

  if (PQresultStatus(aRes) != PGRES_TUPLES_OK)
  {
    KT_E("timescale: troe_attrs SELECT failed: %s", PQerrorMessage(timescaleConn));
    PQclear(aRes);
    return TROE_ERR;
  }

  int  rowN      = PQntuples(aRes);

  if (rowN == 0 && entityType == NULL)
  {
    PQclear(aRes);
    return TROE_NOT_FOUND;
  }

  Kjson*  kjsonP = swRest.kjsonP;
  KjNode* root   = kjObject(kjsonP, NULL);

  kjChildAdd(root, kjString(kjsonP, "id", entityId));
  if (entityType != NULL)
    kjChildAdd(root, kjString(kjsonP, "type", entityType));

  // § 4.5.2 / § 4.5.6 createdAt / modifiedAt at the entity level — derived
  // from troe_entities. createdAt = the modified_at of the earliest 'created'
  // row; modifiedAt = the most recent modified_at across all rows (which
  // also covers append / replace updates). Attached unconditionally; the
  // common renderHook strips them again when ?sysAttrs is not set, so only
  // sysAttrs-aware consumers (incl. ldOrderSort orderBy=createdAt /
  // modifiedAt) actually see them.
  {
    const char* idParam[2] = { entityId, tenant };
    PGresult* tRes = PQexecParams(timescaleConn,
      "SELECT to_char(MIN(modified_at) FILTER (WHERE op = 'created') "
      "       AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
      "       to_char(MAX(modified_at) AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
      "FROM troe_entities WHERE entity_id = $1 AND tenant = $2",
      2, NULL, idParam, NULL, NULL, 0);
    if (PQresultStatus(tRes) == PGRES_TUPLES_OK && PQntuples(tRes) > 0)
    {
      if (!PQgetisnull(tRes, 0, 0))
        kjChildAdd(root, kjString(kjsonP, "createdAt",
                                   stripZeroMs(kaStrdup(&swRest.kalloc, PQgetvalue(tRes, 0, 0)))));
      if (!PQgetisnull(tRes, 0, 1))
        kjChildAdd(root, kjString(kjsonP, "modifiedAt",
                                   stripZeroMs(kaStrdup(&swRest.kalloc, PQgetvalue(tRes, 0, 1)))));
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

    KjNode* arr = kjLookup(root, attrName);
    if (arr == NULL)
    {
      arr = kjArray(kjsonP, kaStrdup(&swRest.kalloc, attrName));
      kjChildAdd(root, arr);
    }

    KjNode* inst = kjObject(kjsonP, NULL);
    kjChildAdd(inst, kjString(kjsonP, "type", kindToTypeString(attrKind)));

    // § 5.3.2.5: a deleted instance keeps the Attribute's type; its
    // value-field carries the NGSI-LD Null. LanguageProperty nulls take the
    // object form {"@none": "urn:ngsi-ld:null"} (§ 5.4.1).
    bool isDeleted = (strcmp(opStr, "deleted") == 0);

    if (isDeleted)
    {
      const char* vfn = kindValueFieldName(attrKind);
      if (attrKind == LdAttrLanguageProperty)
      {
        KjNode* lmP = kjObject(kjsonP, vfn);
        kjChildAdd(lmP, kjString(kjsonP, "@none", "urn:ngsi-ld:null"));
        kjChildAdd(inst, lmP);
      }
      else
        kjChildAdd(inst, kjString(kjsonP, vfn, "urn:ngsi-ld:null"));
    }
    else
    {
      const char* vfn = kindValueFieldName(attrKind);
      KjNode* vNode = makeValueNode(kjsonP, vfn, v_text, v_number, v_bool, v_compnd);
      if (vNode != NULL)
        kjChildAdd(inst, vNode);
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
      kjChildAdd(inst, kjString(kjsonP, "createdAt",  stripZeroMs(kaStrdup(&swRest.kalloc, crAtIso))));
    if (modAtIso != NULL)
      kjChildAdd(inst, kjString(kjsonP, "modifiedAt", stripZeroMs(kaStrdup(&swRest.kalloc, modAtIso))));
    if (isDeleted && modAtIso != NULL)
      kjChildAdd(inst, kjString(kjsonP, "deletedAt",  stripZeroMs(kaStrdup(&swRest.kalloc, modAtIso))));
    if (obsAtIso != NULL)
      kjChildAdd(inst, kjString(kjsonP, "observedAt", stripZeroMs(kaStrdup(&swRest.kalloc, obsAtIso))));
    if (dsId != NULL && dsId[0] != 0)
      kjChildAdd(inst, kjString(kjsonP, "datasetId", kaStrdup(&swRest.kalloc, dsId)));
    if (instId != NULL)
      kjChildAdd(inst, kjString(kjsonP, "instanceId", kaStrdup(&swRest.kalloc, instId)));

    if (subAttrs != NULL && subAttrs[0] != 0)
    {
      char* dup = kaStrdup(&swRest.kalloc, subAttrs);
      KjNode* parsed = kjParse(kjsonP, dup);
      if (parsed != NULL && parsed->type == KjObject)
      {
        for (KjNode* sP = parsed->value.firstChildP; sP != NULL; sP = sP->next)
          kjChildAdd(inst, sP);
      }
    }

    kjChildAdd(arr, inst);
  }

  // Accumulate range info for the caller (multi-entity unions, single-entity
  // takes its values straight). Only the first writer fills size — it stays
  // constant across entities for one request.
  // minIso / maxIso point straight into aRes' libpq-owned storage, so
  // kaStrdup BEFORE the PQclear that frees them.
  if (rangeOut != NULL)
  {
    if (hasMore)
      rangeOut->hasMore = true;

    if (minIso != NULL)
    {
      if (rangeOut->rangeStartIso == NULL || strcmp(minIso, rangeOut->rangeStartIso) < 0)
        rangeOut->rangeStartIso = stripZeroMs(kaStrdup(&swRest.kalloc, minIso));
    }
    if (maxIso != NULL)
    {
      if (rangeOut->rangeEndIso == NULL || strcmp(maxIso, rangeOut->rangeEndIso) > 0)
        rangeOut->rangeEndIso = stripZeroMs(kaStrdup(&swRest.kalloc, maxIso));
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
                                    TroeQueryFilter* fP, KjNode** resultPP,
                                    TroeRangeInfo* rangeOut)
{
  if (timescaleConn == NULL || entityId == NULL || resultPP == NULL)
    return TROE_ERR;

  *resultPP = NULL;

  const char* tenant = (tenantP != NULL) ? tenantP->name : "";

  pthread_mutex_lock(&timescaleMutex);
  int r = buildEntityTemporalDocLocked(tenant, entityId, NULL, fP, resultPP, rangeOut);
  pthread_mutex_unlock(&timescaleMutex);

  return r;
}



// -----------------------------------------------------------------------------
//
// idsInClause - " AND entity_id IN ('a','b',...)" or "" when idV is empty.
//
static const char* idsInClause(char** idV, KAlloc* kaP)
{
  if (idV == NULL || idV[0] == NULL)
    return "";

  int needed = 32;
  for (int i = 0; idV[i] != NULL; i++)
    needed += (int) strlen(idV[i]) * 2 + 4;

  char* buf = (char*) kaAlloc(kaP, needed);
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
// typesInClause - " AND entity_type IN ('a','b',...)" or "" when empty.
//
static const char* typesInClause(char** typeV, KAlloc* kaP)
{
  if (typeV == NULL || typeV[0] == NULL)
    return "";

  int needed = 32;
  for (int i = 0; typeV[i] != NULL; i++)
    needed += (int) strlen(typeV[i]) * 2 + 4;

  char* buf = (char*) kaAlloc(kaP, needed);
  int   p   = 0;
  p += snprintf(buf + p, needed - p, " AND entity_type IN (");

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
  buf[p++] = ')';
  buf[p]   = 0;
  return buf;
}



// -----------------------------------------------------------------------------
//
// idPatternClause - " AND entity_id ~ '<pattern>'" or "".
//
static const char* idPatternClause(const char* idPattern, KAlloc* kaP)
{
  if (idPattern == NULL || idPattern[0] == 0)
    return "";

  int   sz  = (int) strlen(idPattern) * 2 + 32;
  char* buf = (char*) kaAlloc(kaP, sz);
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
// timescaleEntityTemporalQuery - § 5.7.4 multi-entity query.
//
// Selects candidate entity ids matching the entity-level filters
// (id / idPattern / type), then builds an EntityTemporal doc per
// candidate via the shared helper. q (when present) is enforced
// per-entity inside the helper (N+1 reads — fine for v1).
//
int timescaleEntityTemporalQuery(Tenant* tenantP, TroeQueryFilter* fP,
                                 KjNode** resultPP, TroeRangeInfo* rangeOut)
{
  if (timescaleConn == NULL || fP == NULL || resultPP == NULL)
    return TROE_ERR;

  *resultPP = NULL;

  const char* tenant = (tenantP != NULL) ? tenantP->name : "";

  Kjson*  kjsonP = swRest.kjsonP;
  KjNode* arrP   = kjArray(kjsonP, NULL);

  // Entity-selector predicates.
  const char* idClause      = idsInClause(fP->idV, &swRest.kalloc);
  const char* typeClause    = typesInClause(fP->typeV, &swRest.kalloc);
  const char* patternClause = idPatternClause(fP->idPattern, &swRest.kalloc);

  // limit/offset — caller guarantees non-negative; default broker-side.
  // The LIMIT is applied AFTER per-candidate doc construction so we can
  // skip entities whose doc has no temporal content (post-filter empty),
  // rather than letting them eat into the limit. SQL still pre-narrows
  // by the entity-level predicates and keeps the candidate count
  // bounded — entities with no troe_entities row are already excluded.
  int limit  = (fP->limit > 0) ? fP->limit : 1000;
  int offset = (fP->offset > 0) ? fP->offset : 0;

  // DISTINCT ON to collapse the multiple rows per entity (creates +
  // replaces) to the most-recent (entity_id, entity_type) pair.
  // Tenant-scoped: $1 = tenant.
  int   sqlSize = 4096;
  char* sql     = (char*) kaAlloc(&swRest.kalloc, sqlSize);
  snprintf(sql, sqlSize,
    "SELECT entity_id, entity_type FROM ("
    "SELECT DISTINCT ON (entity_id) entity_id, entity_type, modified_at "
    "FROM troe_entities WHERE tenant = $1 ORDER BY entity_id, modified_at DESC"
    ") latest "
    "WHERE TRUE%s%s%s "
    "ORDER BY entity_id",
    idClause, typeClause, patternClause);

  const char* selectorParamV[1] = { tenant };

  pthread_mutex_lock(&timescaleMutex);

  PGresult* eRes = PQexecParams(timescaleConn, sql, 1, NULL, selectorParamV, NULL, NULL, 0);
  if (PQresultStatus(eRes) != PGRES_TUPLES_OK)
  {
    KT_E("timescale: entity-selector SELECT failed: %s", PQerrorMessage(timescaleConn));
    PQclear(eRes);
    pthread_mutex_unlock(&timescaleMutex);
    return TROE_ERR;
  }

  int candN = PQntuples(eRes);

  // Track how many filter-passing entities we've seen; offset skips
  // the first N matches; limit caps the total kept.
  int matched = 0;
  int kept    = 0;

  for (int r = 0; r < candN && kept < limit; r++)
  {
    const char* entityId   = kaStrdup(&swRest.kalloc, PQgetvalue(eRes, r, 0));
    const char* entityType = PQgetisnull(eRes, r, 1) ? NULL : kaStrdup(&swRest.kalloc, PQgetvalue(eRes, r, 1));

    KjNode* docP = NULL;
    int rc = buildEntityTemporalDocLocked(tenant, entityId, entityType, fP, &docP, rangeOut);

    if (rc == TROE_ERR)
    {
      PQclear(eRes);
      pthread_mutex_unlock(&timescaleMutex);
      return TROE_ERR;
    }
    if (rc != TROE_OK || docP == NULL)
      continue;   // TROE_NOT_FOUND — entity didn't match q/etc

    // Drop entities whose doc has no temporal-attribute content. An
    // entity that's known to TRoE but has no instances matching the
    // query (timerel / attrs / datasetId / q) is not part of the result
    // set per § 5.7.4. Without this, leftover rows in troe_entities
    // surface as bare {id,type} stubs.
    bool hasAttrs = false;
    for (KjNode* c = docP->value.firstChildP; c != NULL; c = c->next)
    {
      if (c->name == NULL)                         continue;
      if (strcmp(c->name, "id")         == 0)      continue;
      if (strcmp(c->name, "type")       == 0)      continue;
      if (strcmp(c->name, "scope")      == 0)      continue;
      // sysAttrs surfaced for orderBy — not user attributes.
      if (strcmp(c->name, "createdAt")  == 0)      continue;
      if (strcmp(c->name, "modifiedAt") == 0)      continue;
      hasAttrs = true;
      break;
    }
    if (!hasAttrs)
      continue;

    if (matched++ < offset)
      continue;

    kjChildAdd(arrP, docP);
    kept++;
  }

  PQclear(eRes);
  pthread_mutex_unlock(&timescaleMutex);

  *resultPP = arrP;
  return TROE_OK;
}
