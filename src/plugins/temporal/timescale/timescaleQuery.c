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
int timescaleEntityTemporalRetrieve(Tenant* tenantP, const char* entityId,
                                    TroeQueryFilter* fP, KjNode** resultPP)
{
  (void) tenantP;
  (void) fP;

  if (timescaleConn == NULL || entityId == NULL || resultPP == NULL)
    return TROE_ERR;

  *resultPP = NULL;

  pthread_mutex_lock(&timescaleMutex);

  // -------------------------------------------------------------------------
  //
  // Pull both:
  //   - the most recent entity-level row (for entity_type)
  //   - all attr rows
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

  PGresult* aRes = PQexecParams(timescaleConn,
    "SELECT attr_name, attr_kind, dataset_id, "
    "       to_char(modified_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
    "       to_char(observed_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), "
    "       op, v_text, v_number, v_bool, v_compound, sub_attrs "
    "FROM troe_attrs "
    "WHERE entity_id = $1 "
    "ORDER BY modified_at",
    1, NULL, idParam, NULL, NULL, 0);

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
