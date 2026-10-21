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
// tsBufLiteral -
//
bool tsBufLiteral(TsBuf* bP, const char* s, const char* cast)
{
  if (s == NULL)
    return (cast != NULL) ? tsBufText(bP, "NULL::%s", cast) : tsBufAppend(bP, "NULL", 4);

  int n = (int) strlen(s);

  // Worst case every byte a quote: 2n, plus the quotes, plus the cast
  if (tsBufRoom(bP, 2 * n + 2 + ((cast != NULL) ? (int) strlen(cast) + 2 : 0)) == false)
    return false;

  char* outP = &bP->s[bP->len];

  *outP++ = '\'';
  for (const char* cP = s; *cP != 0; cP++)
  {
    if (*cP == '\'')
      *outP++ = '\'';
    *outP++ = *cP;
  }
  *outP++ = '\'';

  bP->len = (int) (outP - bP->s);
  bP->s[bP->len] = 0;

  return (cast != NULL) ? tsBufText(bP, "::%s", cast) : true;
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

  free(uP->entityRows.s);
  free(uP->attrRows.s);
  free(uP);
}



// -----------------------------------------------------------------------------
//
// tsUnitBytes -
//
int tsUnitBytes(const TsUnit* uP)
{
  return uP->entityRows.len + uP->attrRows.len;
}



// -----------------------------------------------------------------------------
//
// rowsAppend - the INSERT of one table: head, the rows of every unit, tail
//
static bool rowsAppend(TsBuf* sqlP, TsUnit** unitV, int unitN, bool entities, const char* head, const char* tail)
{
  bool first = true;

  for (int ix = 0; ix < unitN; ix++)
  {
    TsBuf* rowsP = (entities == true) ? &unitV[ix]->entityRows : &unitV[ix]->attrRows;

    if (rowsP->len == 0)
      continue;

    if (tsBufText(sqlP, "%s", (first == true) ? head : ",") == false)
      return false;
    if (tsBufAppend(sqlP, rowsP->s, rowsP->len) == false)
      return false;

    first = false;
  }

  return (first == true) ? true : tsBufText(sqlP, "%s;", tail);
}



// -----------------------------------------------------------------------------
//
// timescaleUnitsExec -
//
int timescaleUnitsExec(PGconn* conn, TsUnit** unitV, int unitN)
{
  TsBuf sql = { NULL, 0, 0 };
  bool  ok  = true;

  ok = ok && rowsAppend(&sql, unitV, unitN, true,
                        "INSERT INTO troe_entities (entity_id, entity_type, modified_at, op) VALUES ",
                        " ON CONFLICT (entity_id, modified_at, op) DO NOTHING");
  ok = ok && rowsAppend(&sql, unitV, unitN, false,
                        "INSERT INTO troe_attrs (instance_id, entity_id, entity_type, attr_name, dataset_id, created_at, modified_at,"
                        " op, attr_kind, observed_at, v_text, v_number, v_bool, v_datetime, v_compound, sub_attrs) VALUES ",
                        "");

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
