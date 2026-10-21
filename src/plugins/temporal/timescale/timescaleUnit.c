//
// FILE            timescaleUnit.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The rows of an event list as SQL text - see timescaleUnit.h.
//

#include <stdarg.h>                                       // va_list
#include <stdbool.h>                                      // bool
#include <stddef.h>                                       // NULL
#include <stdio.h>                                        // vsnprintf
#include <stdlib.h>                                       // calloc, free, realloc
#include <string.h>                                       // memcpy, strlen
#include <libpq-fe.h>                                     // PG*

#include "corLog/corLog.h"                                // COR_E

#include "troe/TroeDriver.h"                              // TROE_OK, TROE_ERR

#include "temporal/timescale/timescaleUnit.h"             // Own interface



// -----------------------------------------------------------------------------
//
// tsBufRoom - make room for n more bytes (and a terminator)
//
static bool tsBufRoom(TsBuf* bP, int n)
{
  if (bP->len + n + 1 <= bP->size)
    return true;

  int   size = bP->size * 2 + n + 1024;
  char* s    = (char*) realloc(bP->s, size);

  if (s == NULL)
    return false;

  bP->s    = s;
  bP->size = size;
  return true;
}



// -----------------------------------------------------------------------------
//
// tsBufAppend -
//
static bool tsBufAppend(TsBuf* bP, const char* s, int n)
{
  if (tsBufRoom(bP, n) == false)
    return false;

  memcpy(&bP->s[bP->len], s, n);
  bP->len += n;
  bP->s[bP->len] = 0;
  return true;
}



// -----------------------------------------------------------------------------
//
// tsBufText -
//
bool tsBufText(TsBuf* bP, const char* fmt, ...)
{
  while (true)
  {
    int     avail = bP->size - bP->len;
    va_list ap;

    va_start(ap, fmt);
    int n = (avail > 0) ? vsnprintf(&bP->s[bP->len], avail, fmt, ap) : vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0)
      return false;

    if (n < avail)
    {
      bP->len += n;
      return true;
    }

    if (tsBufRoom(bP, n) == false)
      return false;
  }
}



// -----------------------------------------------------------------------------
//
// tsBufElement -
//
bool tsBufElement(TsBuf* bP, bool first, const char* s)
{
  if (s == NULL)
    return tsBufAppend(bP, (first == true) ? "NULL" : ",NULL", (first == true) ? 4 : 5);

  int n = (int) strlen(s);

  // Worst case every byte a quote: 2n, plus the comma and the element's quotes
  if (tsBufRoom(bP, 2 * n + 3) == false)
    return false;

  char* outP = &bP->s[bP->len];

  if (first == false)
    *outP++ = ',';

  *outP++ = '"';
  for (const char* cP = s; *cP != 0; cP++)
  {
    if      (*cP == '\'') *outP++ = '\'';    // SQL
    else if (*cP == '"')  *outP++ = '\\';    // array
    else if (*cP == '\\') *outP++ = '\\';    // array
    *outP++ = *cP;
  }
  *outP++ = '"';

  bP->len = (int) (outP - bP->s);
  bP->s[bP->len] = 0;

  return true;
}



// -----------------------------------------------------------------------------
//
// tsUnitNew -
//
TsUnit* tsUnitNew(void)
{
  return (TsUnit*) calloc(1, sizeof(TsUnit));
}



// -----------------------------------------------------------------------------
//
// tsUnitFree -
//
void tsUnitFree(TsUnit* uP)
{
  if (uP == NULL)
    return;

  for (unsigned int ix = 0; ix < sizeof(uP->entityCol) / sizeof(uP->entityCol[0]); ix++)
    free(uP->entityCol[ix].s);
  for (unsigned int ix = 0; ix < sizeof(uP->attrCol) / sizeof(uP->attrCol[0]); ix++)
    free(uP->attrCol[ix].s);
  free(uP);
}



// -----------------------------------------------------------------------------
//
// tsUnitBytes -
//
int tsUnitBytes(const TsUnit* uP)
{
  int bytes = 0;

  for (unsigned int ix = 0; ix < sizeof(uP->entityCol) / sizeof(uP->entityCol[0]); ix++)
    bytes += uP->entityCol[ix].len;
  for (unsigned int ix = 0; ix < sizeof(uP->attrCol) / sizeof(uP->attrCol[0]); ix++)
    bytes += uP->attrCol[ix].len;

  return bytes;
}



// -----------------------------------------------------------------------------
//
// insertAppend - the INSERT of one table: head, an array per column - the elements of every unit's
// rows - and tail
//
// colTypeV: the array types of the columns, in the order of the unnest in head.
//
static bool insertAppend(TsBuf* sqlP, TsUnit** unitV, int unitN, bool entities, const char* head, const char** colTypeV, int colN, const char* tail)
{
  bool any = false;

  for (int ix = 0; ix < unitN; ix++)
  {
    if (((entities == true) ? unitV[ix]->entityRowN : unitV[ix]->attrRowN) > 0)
      any = true;
  }

  if (any == false)
    return true;

  if (tsBufText(sqlP, "%s", head) == false)
    return false;

  for (int colIx = 0; colIx < colN; colIx++)
  {
    bool first = true;

    if (tsBufAppend(sqlP, (colIx == 0) ? "'{" : ", '{", (colIx == 0) ? 2 : 4) == false)
      return false;

    for (int ix = 0; ix < unitN; ix++)
    {
      TsBuf* colP = (entities == true) ? &unitV[ix]->entityCol[colIx] : &unitV[ix]->attrCol[colIx];

      if (colP->len == 0)
        continue;

      if ((first == false) && (tsBufAppend(sqlP, ",", 1) == false))
        return false;
      if (tsBufAppend(sqlP, colP->s, colP->len) == false)
        return false;

      first = false;
    }

    if (tsBufText(sqlP, "}'::%s", colTypeV[colIx]) == false)
      return false;
  }

  return tsBufText(sqlP, "%s;", tail);
}



// -----------------------------------------------------------------------------
//
// timescaleUnitsExec -
//
// troe_attrs' instance_id: an imported instance brings its own, unique in the store it came from;
// otherwise it is generated - the column's DEFAULT (timescaleMigrate.c), which an INSERT ... SELECT
// cannot name per row - so each row is unique by construction and there is no ON CONFLICT.
//
// entity_type: an attribute event carries no entity snapshot, so the type can come out empty for a
// multi-type Entity; the type list already recorded for this Entity then, rather than storing nothing.
// COALESCE evaluates the subquery only for such a row.
//
int timescaleUnitsExec(PGconn* conn, TsUnit** unitV, int unitN)
{
  static const char* entityColTypeV[] = { "text[]", "text[]", "float8[]", "text[]" };
  static const char* attrColTypeV[]   = { "text[]", "text[]", "text[]", "text[]", "text[]", "float8[]", "float8[]", "text[]",
                                          "smallint[]", "timestamptz[]", "text[]", "float8[]", "bool[]", "timestamptz[]",
                                          "text[]", "text[]" };
  TsBuf sql = { NULL, 0, 0 };
  bool  ok  = true;

  ok = ok && insertAppend(&sql, unitV, unitN, true,
                          "INSERT INTO troe_entities (entity_id, entity_type, modified_at, op)"
                          " SELECT e, t::text[], to_timestamp(m), o FROM unnest(",
                          entityColTypeV, sizeof(entityColTypeV) / sizeof(entityColTypeV[0]),
                          ") AS u(e, t, m, o) ON CONFLICT (entity_id, modified_at, op) DO NOTHING");
  ok = ok && insertAppend(&sql, unitV, unitN, false,
                          "INSERT INTO troe_attrs (instance_id, entity_id, entity_type, attr_name, dataset_id, created_at, modified_at,"
                          " op, attr_kind, observed_at, v_text, v_number, v_bool, v_datetime, v_compound, sub_attrs)"
                          " SELECT COALESCE(i, 'urn:ngsi-ld:Instance:' || gen_random_uuid()), e,"
                          " COALESCE(NULLIF(t::text[], '{}'),"
                          " (SELECT entity_type FROM troe_entities WHERE entity_id = e ORDER BY modified_at DESC LIMIT 1),"
                          " '{}'::text[]),"
                          " a, d, to_timestamp(c), to_timestamp(m), o, k, ob, vt, vn, vb, vd, vc::jsonb, sa::jsonb FROM unnest(",
                          attrColTypeV, sizeof(attrColTypeV) / sizeof(attrColTypeV[0]),
                          ") AS u(i, e, t, a, d, c, m, o, k, ob, vt, vn, vb, vd, vc, sa)");

  if (ok == false)
  {
    COR_E("timescale: TRoE INSERT: out of memory");
    free(sql.s);
    return TROE_ERR;
  }

  if (sql.len == 0)
    return TROE_OK;

  int       rc  = TROE_OK;
  PGresult* res = PQexec(conn, sql.s);

  if (PQresultStatus(res) != PGRES_COMMAND_OK)
  {
    COR_E("timescale: TRoE INSERT failed: %s", PQerrorMessage(conn));
    rc = TROE_ERR;
  }

  PQclear(res);
  free(sql.s);

  return rc;
}
