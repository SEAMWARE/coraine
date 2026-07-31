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
#include <libpq-fe.h>                                     // PG*

#include "ktrace/kTrace.h"                                // KT_E
#include "kjson/kjBuilder.h"                              // kjObject, kjArray, kjString, kjInteger, kjFloat, kjBoolean, kjChildAdd
#include "kjson/kjLookup.h"                               // kjLookup
#include "kjson/kjParse.h"                                // kjParse
#include "kalloc/kaAlloc.h"                               // kaAlloc
#include "kalloc/kaStrdup.h"                              // kaStrdup

#include "swRest/SwRestState.h"                           // swRest
#include "swNgsild/LdAttrType.h"                          // LdAttr*
#include "swNgsild/LdGeoRel.h"                            // LdGeoNear, LdGeoRelType
#include "swNgsild/SwNgsild.h"                            // swNgsild

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
  char* sql = (char*) kaAlloc(&swRest.kalloc, sz);
  snprintf(sql, sz, "SELECT %s", qPred);

  PGresult* res = PQexecParams(timescaleConn, sql, 1, NULL, idParam, NULL, NULL, 0);
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
// typeNodeFromJson - build the entity's "type" member from the JSON array the
// SELECT returns for the TEXT[] column, e.g. ["Vehicle","Car"].
//
// § 5.2.6.4.2: one type name renders as a JSON string, several as an array. A
// missing or empty list yields NULL and the member is left out entirely, which
// is what an Entity written before the column became an array looks like.
//
static KjNode* typeNodeFromJson(const char* json, Kjson* kjsonP, KAlloc* kaP)
{
  if ((json == NULL) || (json[0] == 0))
    return NULL;

  char*   copy  = kaStrdup(kaP, json);
  KjNode* arrayP = kjParse(kjsonP, copy);

  if ((arrayP == NULL) || (arrayP->type != KjArray) || (arrayP->value.firstChildP == NULL))
    return NULL;

  KjNode* firstP = arrayP->value.firstChildP;

  if (firstP->next == NULL)
  {
    if ((firstP->type != KjString) || (firstP->value.s == NULL) || (firstP->value.s[0] == 0))
      return NULL;
    return kjString(kjsonP, "type", firstP->value.s);
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
      // kjParse turns straight into the node the renderer wants.
      "SELECT array_to_json(entity_type)::text FROM troe_entities "
      "WHERE entity_id = $1 "
      "ORDER BY modified_at DESC LIMIT 1",
      1, NULL, idParam, NULL, NULL, 0);
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
      char* buf = (char*) kaAlloc(&swRest.kalloc, 64);
      snprintf(buf, 64, " AND %s < $2::timestamptz", tCol);
      timePred = buf;
      paramV[1] = timeAt;
      nParams   = 2;
    }
    else if (strcmp(timerel, "after") == 0)
    {
      char* buf = (char*) kaAlloc(&swRest.kalloc, 64);
      snprintf(buf, 64, " AND %s >= $2::timestamptz", tCol);
      timePred = buf;
      paramV[1] = timeAt;
      nParams   = 2;
    }
    else if (strcmp(timerel, "between") == 0)
    {
      char* buf = (char*) kaAlloc(&swRest.kalloc, 96);
      snprintf(buf, 96, " AND %s >= $2::timestamptz AND %s < $3::timestamptz", tCol, tCol);
      timePred = buf;
      paramV[1] = timeAt;
      paramV[2] = endTimeAt;
      nParams   = 3;
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
    "FROM troe_attrs WHERE entity_id = $1%s%s%s%s "
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
    "WHERE entity_id = $1%s%s%s%s) sub "
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

  KjNode* typeNodeP = typeNodeFromJson(entityType, kjsonP, &swRest.kalloc);
  if (typeNodeP != NULL)
    kjChildAdd(root, typeNodeP);

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
      "       AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
      "       to_char(MAX(modified_at) AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') "
      "FROM troe_entities WHERE entity_id = $1",
      1, NULL, idParam, NULL, NULL, 0);
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
// typesInClause - " AND entity_type && ARRAY['a','b',...]" or "" when empty.
//
// entity_type is a TEXT[] (an Entity may hold several types, § 5.2.6.4.2), so
// the filter is an array-OVERLAP test: the Entity matches when ANY of its types
// is among the requested ones. The GIN index on entity_type serves &&.
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
// correlateQPred - rewrite a compiled q-predicate for use as a correlated
// subquery in the entity-selector. troeQTreeToSql emits the outer entity_id
// as the bind parameter $1 (its only parameter — value literals are inlined);
// for the set query we correlate it to the selector's row instead, i.e.
// replace every "$1" with "latest.entity_id".
//
static const char* correlateQPred(const char* qPred, KAlloc* kaP)
{
  int   sz  = (int) strlen(qPred) + 64;   // "latest.entity_id" is longer than "$1"
  // Each "$1" (2 chars) grows to 16 chars → +14 per occurrence; bound generously.
  for (const char* s = qPred; *s != 0; s++)
    if (s[0] == '$' && s[1] == '1')
      sz += 16;

  char* buf = (char*) kaAlloc(kaP, sz);
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
                                          KAlloc* kaP)
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
  char* buf = (char*) kaAlloc(kaP, sz);
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
                                 KjNode** resultPP, TroeRangeInfo* rangeOut)
{
  if (fP == NULL || resultPP == NULL)
    return TROE_ERR;

  *resultPP = NULL;
  rangeOut->entityCount = -1;     // not computed unless ?count

  TimescaleConn* cP = timescaleConnGet(tenantP);
  if (cP == NULL) return TROE_ERR;
  timescaleConn = cP->conn;

  Kjson*  kjsonP = swRest.kjsonP;
  KjNode* arrP   = kjArray(kjsonP, NULL);

  // limitGiven distinguishes an explicit limit=0 (count-only page) from an
  // absent limit (broker default). limit=0 → fetch LIMIT 1 (to set
  // moreEntities) but build 0 docs; the COUNT(*) still runs under ?count.
  int limit  = fP->limitGiven ? fP->limit : 1000;
  int offset = (fP->offset > 0) ? fP->offset : 0;

  // Entity-level selectors.
  const char* idClause      = idsInClause(fP->idV, &swRest.kalloc);
  const char* typeClause    = typesInClause(fP->typeV, &swRest.kalloc);
  const char* patternClause = idPatternClause(fP->idPattern, &swRest.kalloc);

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
  const char* attrPred = attrsInClause(fP->attrV, &swRest.kalloc);
  const char* dsPred   = datasetIdsInClause(fP->datasetIdV, &swRest.kalloc);

  const char* timePred = "";
  int         nParams  = 0;
  const char* paramV[2];
  if (fP->timerel != NULL)
  {
    if (strcmp(fP->timerel, "before") == 0)
    {
      char* b = (char*) kaAlloc(&swRest.kalloc, 64);
      snprintf(b, 64, " AND %s < $1::timestamptz", tCol);
      timePred = b; paramV[0] = fP->timeAtIso; nParams = 1;
    }
    else if (strcmp(fP->timerel, "after") == 0)
    {
      char* b = (char*) kaAlloc(&swRest.kalloc, 64);
      snprintf(b, 64, " AND %s >= $1::timestamptz", tCol);
      timePred = b; paramV[0] = fP->timeAtIso; nParams = 1;
    }
    else if (strcmp(fP->timerel, "between") == 0)
    {
      char* b = (char*) kaAlloc(&swRest.kalloc, 96);
      snprintf(b, 96, " AND %s >= $1::timestamptz AND %s < $2::timestamptz", tCol, tCol);
      timePred = b; paramV[0] = fP->timeAtIso; paramV[1] = fP->endTimeAtIso; nParams = 2;
    }
  }

  // Correlated q predicate (entity-level precondition).
  const char* qCorr = "";
  if (fP->qSqlPredicate != NULL)
  {
    const char* c  = correlateQPred(fP->qSqlPredicate, &swRest.kalloc);
    int         sz = (int) strlen(c) + 8;
    char*       b  = (char*) kaAlloc(&swRest.kalloc, sz);
    snprintf(b, sz, " AND %s", c);
    qCorr = b;
  }

  // Correlated geo predicate (§ 11.3.3) — pushes ?georel near into SQL.
  const char* geoPred = geoPredicateCorrelated(fP, tCol, timePred, opPred, &swRest.kalloc);

  // WHERE body shared by the page query and the count query.
  int   wSize = 16384;
  char* where = (char*) kaAlloc(&swRest.kalloc, wSize);
  snprintf(where, wSize,
    "FROM (SELECT DISTINCT ON (entity_id) entity_id, entity_type, modified_at "
    "FROM troe_entities ORDER BY entity_id, modified_at DESC) latest "
    "WHERE TRUE%s%s%s "
    "AND EXISTS (SELECT 1 FROM troe_attrs WHERE entity_id = latest.entity_id%s%s%s%s)%s%s",
    idClause, typeClause, patternClause, timePred, opPred, attrPred, dsPred, qCorr, geoPred);

  // Page query — fetch limit+1 so we can tell whether more entities remain.
  int   pSize = wSize + 256;
  char* pageSql = (char*) kaAlloc(&swRest.kalloc, pSize);
  snprintf(pageSql, pSize,
    "SELECT entity_id, array_to_json(entity_type)::text %s ORDER BY entity_id LIMIT %d OFFSET %d",
    where, limit + 1, offset);

  PGresult* eRes = PQexecParams(timescaleConn, pageSql, nParams, NULL,
                                (nParams > 0) ? paramV : NULL, NULL, NULL, 0);
  if (PQresultStatus(eRes) != PGRES_TUPLES_OK)
  {
    KT_E("timescale: entity-selector SELECT failed: %s", PQerrorMessage(timescaleConn));
    PQclear(eRes);
    timescaleConn = NULL;
    timescaleConnRelease(cP);
    return TROE_ERR;
  }

  int rowN  = PQntuples(eRes);
  int pageN = (rowN > limit) ? limit : rowN;     // the extra row only signals "more"
  rangeOut->moreEntities = (rowN > limit);

  for (int r = 0; r < pageN; r++)
  {
    const char* entityId   = kaStrdup(&swRest.kalloc, PQgetvalue(eRes, r, 0));
    const char* entityType = PQgetisnull(eRes, r, 1) ? NULL : kaStrdup(&swRest.kalloc, PQgetvalue(eRes, r, 1));

    KjNode* docP = NULL;
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
    for (KjNode* c = docP->value.firstChildP; c != NULL; c = c->next)
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

    kjChildAdd(arrP, docP);
  }

  PQclear(eRes);

  // § 6.4.6 count — total matching entities over the same WHERE (no LIMIT).
  if (fP->count)
  {
    int   cSize = wSize + 64;
    char* countSql = (char*) kaAlloc(&swRest.kalloc, cSize);
    snprintf(countSql, cSize, "SELECT COUNT(*) %s", where);

    PGresult* nRes = PQexecParams(timescaleConn, countSql, nParams, NULL,
                                  (nParams > 0) ? paramV : NULL, NULL, NULL, 0);
    if (PQresultStatus(nRes) == PGRES_TUPLES_OK && PQntuples(nRes) > 0)
      rangeOut->entityCount = strtol(PQgetvalue(nRes, 0, 0), NULL, 10);
    else
      KT_E("timescale: entity-count SELECT failed: %s", PQerrorMessage(timescaleConn));
    PQclear(nRes);
  }

  timescaleConn = NULL;
  timescaleConnRelease(cP);

  *resultPP = arrP;
  return TROE_OK;
}
