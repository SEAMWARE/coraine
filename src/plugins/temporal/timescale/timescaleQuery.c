//
// FILE            timescaleQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Read path. v1 retrieves the full temporal evolution of one entity —
// no time-window / q / pick filtering yet (those come from
// TroeQueryFilter as it grows).
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

#include "troe/TroeDriver.h"                              // TroeQueryFilter, TROE_*

#include "temporal/timescale/timescaleGlobals.h"          // timescaleConn, timescaleMutex
#include "temporal/timescale/timescaleQuery.h"            // Own interface



// -----------------------------------------------------------------------------
//
// kindToTypeString - attr_kind smallint → NGSI-LD type string.
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
// kindValueFieldName - inverse of the storage normalization.
//
// For each attr kind, NGSI-LD has a different value field name in the
// API representation: Relationship → "object", LanguageProperty →
// "languageMap", etc. Storage always uses "value"; we re-expand here.
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
    /* Property, GeoProperty, default */
    default:                     return "value";
  }
}



// -----------------------------------------------------------------------------
//
// makeValueNode - build the value KjNode from the typed columns.
//
// At most one of v_text / v_number / v_bool / v_compound is non-NULL
// per row (the others come back as PostgreSQL NULL → empty string from
// PQgetvalue + PQgetisnull == 1).
//
static KjNode* makeValueNode(Kjson* kjsonP,
                             const char* fieldName,
                             const char* v_text,
                             const char* v_number,
                             const char* v_bool,
                             const char* v_compound)
{
  if (v_text != NULL)
    return kjString(kjsonP, fieldName, v_text);

  if (v_number != NULL)
  {
    // Try int first; fall back to float if there's a decimal point.
    if (strchr(v_number, '.') != NULL || strchr(v_number, 'e') != NULL)
      return kjFloat(kjsonP, fieldName, strtod(v_number, NULL));
    return kjInteger(kjsonP, fieldName, strtoll(v_number, NULL, 10));
  }

  if (v_bool != NULL)
    return kjBoolean(kjsonP, fieldName, (v_bool[0] == 't' || v_bool[0] == 'T') ? KTRUE : KFALSE);

  if (v_compound != NULL)
  {
    // JSONB came in as a JSON-text string. Re-parse onto the request arena.
    char* dup = kaStrdup(&swRest.kalloc, v_compound);
    KjNode* parsed = kjParse(kjsonP, dup);
    if (parsed != NULL)
    {
      parsed->name = (char*) fieldName;
      return parsed;
    }
  }

  // Op = deleted, or unrecognised — render explicit null for visibility.
  return kjNull(kjsonP, fieldName);
}



// -----------------------------------------------------------------------------
//
// timescaleEntityTemporalRetrieve -
//
// Returns TROE_OK with *resultPP set to a TemporalEntity tree, or
// TROE_NOT_FOUND if the entity has no recorded events.
//
// -----------------------------------------------------------------------------
//
// timeColumn - per ?timeproperty=…, decide which column to filter on.
//
// "observedAt" (default) → observed_at.
// "modifiedAt" / "createdAt" → modified_at (broker-receipt time; createdAt
// gets an additional `op = 'created'` predicate via the caller).
//
static const char* timeColumn(const char* timeproperty)
{
  if (timeproperty == NULL)                          return "observed_at";
  if (strcmp(timeproperty, "modifiedAt") == 0)       return "modified_at";
  if (strcmp(timeproperty, "createdAt")  == 0)       return "modified_at";
  return "observed_at";
}



// -----------------------------------------------------------------------------
//
// attrsInClause - build " AND attr_name IN ('a','b',…)" or "" if no filter.
//
// attrV is a NULL-terminated array of expanded IRIs.
//
static const char* attrsInClause(char** attrV, KAlloc* allocP)
{
  if (attrV == NULL || attrV[0] == NULL)
    return "";

  // Compute size: " AND attr_name IN (" + per-item ('escaped',) + ")"
  int needed = 32;
  for (int i = 0; attrV[i] != NULL; i++)
    needed += (int) strlen(attrV[i]) * 2 + 4;

  char* buf = (char*) kaAlloc(allocP, needed);
  int   p   = 0;
  p += snprintf(buf + p, needed - p, " AND attr_name IN (");
  for (int i = 0; attrV[i] != NULL; i++)
  {
    if (i > 0) buf[p++] = ',';
    buf[p++] = '\'';
    // Escape single quotes by doubling — PG SQL convention.
    for (const char* s = attrV[i]; *s != 0; s++)
    {
      if (*s == '\'') buf[p++] = '\'';
      buf[p++] = *s;
    }
    buf[p++] = '\'';
  }
  buf[p++] = ')';
  buf[p]   = 0;
  return buf;
}



int timescaleEntityTemporalRetrieve(Tenant* tenantP, const char* entityId,
                                    TroeQueryFilter* fP, KjNode** resultPP)
{
  (void) tenantP;

  if (timescaleConn == NULL || entityId == NULL || resultPP == NULL)
    return TROE_ERR;

  *resultPP = NULL;

  // Filter pieces.
  const char* timerel       = (fP != NULL) ? fP->timerel       : NULL;
  const char* timeAt        = (fP != NULL) ? fP->timeAtIso     : NULL;
  const char* endTimeAt     = (fP != NULL) ? fP->endTimeAtIso  : NULL;
  const char* timeProp      = (fP != NULL) ? fP->timeproperty  : NULL;
  char**      attrV         = (fP != NULL) ? fP->attrV         : NULL;
  int         lastN         = (fP != NULL) ? fP->lastN         : 0;
  const char* qPred         = (fP != NULL) ? fP->qSqlPredicate : NULL;

  const char* tCol          = timeColumn(timeProp);
  bool        createdAtOnly = (timeProp != NULL && strcmp(timeProp, "createdAt") == 0);
  const char* opPred        = createdAtOnly ? " AND op = 'created'" : "";
  const char* attrPred      = attrsInClause(attrV, &swRest.kalloc);

  pthread_mutex_lock(&timescaleMutex);

  // -------------------------------------------------------------------------
  //
  // ?q= precondition: does this entity match the q expression at any point
  // in its history? If not, return TROE_NOT_FOUND (404 from the service
  // routine). When qPred is NULL, no q filter — skip.
  //
  if (qPred != NULL)
  {
    const char* idParamCheck[1] = { entityId };
    int   checkSize = (int) strlen(qPred) + 32;
    char* checkSql  = (char*) kaAlloc(&swRest.kalloc, checkSize);
    snprintf(checkSql, checkSize, "SELECT %s", qPred);

    PGresult* qRes = PQexecParams(timescaleConn, checkSql, 1, NULL, idParamCheck, NULL, NULL, 0);
    if (PQresultStatus(qRes) != PGRES_TUPLES_OK)
    {
      KT_E("timescale: q precondition SELECT failed: %s", PQerrorMessage(timescaleConn));
      PQclear(qRes);
      pthread_mutex_unlock(&timescaleMutex);
      return TROE_ERR;
    }
    bool matches = (PQntuples(qRes) == 1
                    && PQgetvalue(qRes, 0, 0) != NULL
                    && PQgetvalue(qRes, 0, 0)[0] == 't');
    PQclear(qRes);
    if (!matches)
    {
      pthread_mutex_unlock(&timescaleMutex);
      return TROE_NOT_FOUND;
    }
  }

  // -------------------------------------------------------------------------
  //
  // entity_type comes from the latest entity-level row — captures replaces.
  //
  const char* idParam[1] = { entityId };

  PGresult* eRes = PQexecParams(timescaleConn,
    "SELECT entity_type FROM troe_entities "
    "WHERE entity_id = $1 ORDER BY modified_at DESC LIMIT 1",
    1, NULL, idParam, NULL, NULL, 0);

  if (PQresultStatus(eRes) != PGRES_TUPLES_OK)
  {
    KT_E("timescale: troe_entities SELECT failed: %s", PQerrorMessage(timescaleConn));
    PQclear(eRes);
    pthread_mutex_unlock(&timescaleMutex);
    return TROE_ERR;
  }

  const char* entityType = NULL;
  if (PQntuples(eRes) > 0)
    entityType = kaStrdup(&swRest.kalloc, PQgetvalue(eRes, 0, 0));
  PQclear(eRes);

  // -------------------------------------------------------------------------
  //
  // Build the attribute-rows query. Three timerel branches × two
  // lastN modes (window-function for lastN, plain ORDER BY otherwise).
  //
  // Time predicate uses the caller's chosen column. tCol is hard-coded
  // (not parameterised) — it's a column name, not data.
  //
  const char* timePred = "";
  int         nParams  = 1;
  const char* paramV[3];
  paramV[0] = entityId;

  if (timerel != NULL)
  {
    if (strcmp(timerel, "before") == 0)
    {
      char* buf = (char*) kaAlloc(&swRest.kalloc, 64);
      snprintf(buf, 64, " AND %s <= $2::timestamptz", tCol);
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
      snprintf(buf, 96, " AND %s >= $2::timestamptz AND %s <= $3::timestamptz", tCol, tCol);
      timePred = buf;
      paramV[1] = timeAt;
      paramV[2] = endTimeAt;
      nParams   = 3;
    }
  }

  // Inner SELECT — to_char-formatted ISOs at positions 3,4 (consumed by
  // the result builder by index); raw timestamps at the end so they're
  // available by name for the outer ORDER BY when lastN wraps in a
  // window.
  const char* selectCols =
    "attr_name, attr_kind, dataset_id, "
    "to_char(modified_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS modified_at_iso, "
    "to_char(observed_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS observed_at_iso, "
    "op, v_text, v_number, v_bool, v_compound, sub_attrs, "
    "modified_at, observed_at";

  // Order-direction follows lastN: "backwards" (DESC) for lastN, otherwise
  // ascending (chronological).
  const char* orderDir = (lastN > 0) ? "DESC" : "ASC";

  int   sqlSize = 4096;
  char* sql     = (char*) kaAlloc(&swRest.kalloc, sqlSize);

  if (lastN > 0)
  {
    // Window function: per (attr_name, dataset_id), keep only the lastN
    // newest rows by tCol. Outer ORDER follows the same axis DESC so the
    // response array reads "most recent first".
    snprintf(sql, sqlSize,
      "SELECT * FROM ("
      "SELECT %s, "
      "       ROW_NUMBER() OVER (PARTITION BY attr_name, dataset_id ORDER BY %s DESC) AS rn "
      "FROM troe_attrs "
      "WHERE entity_id = $1%s%s%s) sub "
      "WHERE rn <= %d "
      "ORDER BY attr_name, dataset_id, %s DESC",
      selectCols, tCol, timePred, opPred, attrPred, lastN, tCol);
  }
  else
  {
    snprintf(sql, sqlSize,
      "SELECT %s "
      "FROM troe_attrs "
      "WHERE entity_id = $1%s%s%s "
      "ORDER BY attr_name, dataset_id, %s %s",
      selectCols, timePred, opPred, attrPred, tCol, orderDir);
  }

  PGresult* aRes = PQexecParams(timescaleConn, sql, nParams, NULL, paramV, NULL, NULL, 0);

  if (PQresultStatus(aRes) != PGRES_TUPLES_OK)
  {
    KT_E("timescale: troe_attrs SELECT failed: %s", PQerrorMessage(timescaleConn));
    PQclear(aRes);
    pthread_mutex_unlock(&timescaleMutex);
    return TROE_ERR;
  }

  int rowN = PQntuples(aRes);

  if (rowN == 0 && entityType == NULL)
  {
    PQclear(aRes);
    pthread_mutex_unlock(&timescaleMutex);
    return TROE_NOT_FOUND;
  }

  // -------------------------------------------------------------------------
  //
  // Build the TemporalEntity tree.
  //
  Kjson*  kjsonP = swRest.kjsonP;
  KjNode* root   = kjObject(kjsonP, NULL);

  kjChildAdd(root, kjString(kjsonP, "id", entityId));
  if (entityType != NULL)
    kjChildAdd(root, kjString(kjsonP, "type", entityType));

  // Walk each row → append an instance to the right per-attr KjArray.
  for (int r = 0; r < rowN; r++)
  {
    const char* attrName  = PQgetvalue(aRes, r, 0);
    int         attrKind  = (int) strtol(PQgetvalue(aRes, r, 1), NULL, 10);
    const char* dsId      = PQgetisnull(aRes, r, 2) ? NULL : PQgetvalue(aRes, r, 2);
    const char* modAtIso  = PQgetisnull(aRes, r, 3) ? NULL : PQgetvalue(aRes, r, 3);
    const char* obsAtIso  = PQgetisnull(aRes, r, 4) ? NULL : PQgetvalue(aRes, r, 4);
    const char* opStr     = PQgetvalue(aRes, r, 5);
    const char* v_text    = PQgetisnull(aRes, r, 6)  ? NULL : PQgetvalue(aRes, r, 6);
    const char* v_number  = PQgetisnull(aRes, r, 7)  ? NULL : PQgetvalue(aRes, r, 7);
    const char* v_bool    = PQgetisnull(aRes, r, 8)  ? NULL : PQgetvalue(aRes, r, 8);
    const char* v_compnd  = PQgetisnull(aRes, r, 9)  ? NULL : PQgetvalue(aRes, r, 9);
    const char* subAttrs  = PQgetisnull(aRes, r, 10) ? NULL : PQgetvalue(aRes, r, 10);

    // Find or create the per-attr array on root.
    KjNode* arr = kjLookup(root, attrName);
    if (arr == NULL)
    {
      arr = kjArray(kjsonP, kaStrdup(&swRest.kalloc, attrName));
      kjChildAdd(root, arr);
    }

    KjNode* inst = kjObject(kjsonP, NULL);
    kjChildAdd(inst, kjString(kjsonP, "type", kindToTypeString(attrKind)));

    // value / object / languageMap / etc. — name depends on kind.
    const char* vfn = kindValueFieldName(attrKind);
    KjNode* vNode = makeValueNode(kjsonP, vfn, v_text, v_number, v_bool, v_compnd);
    if (vNode != NULL)
      kjChildAdd(inst, vNode);

    if (modAtIso != NULL)
      kjChildAdd(inst, kjString(kjsonP, "modifiedAt", kaStrdup(&swRest.kalloc, modAtIso)));
    if (obsAtIso != NULL)
      kjChildAdd(inst, kjString(kjsonP, "observedAt", kaStrdup(&swRest.kalloc, obsAtIso)));
    if (dsId != NULL && dsId[0] != 0)
      kjChildAdd(inst, kjString(kjsonP, "datasetId", kaStrdup(&swRest.kalloc, dsId)));

    // Sub-attrs (JSONB text) — re-parse and splice.
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

    // op marker — surface deletes so consumers can see they happened.
    if (strcmp(opStr, "deleted") == 0)
      kjChildAdd(inst, kjBoolean(kjsonP, "deleted", KTRUE));

    kjChildAdd(arr, inst);
  }

  PQclear(aRes);
  pthread_mutex_unlock(&timescaleMutex);

  *resultPP = root;
  return TROE_OK;
}
