//
// FILE            timescaleIndexes.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The declared indexes of the history (doc/installation.md, "Declared indexes"): an attribute - narrowed
// to an entity type, to a tenant, if so declared - whose q value terms an index of troe_attrs serves.
// Declared at start (--troeDbIndexes) or through the admin API (/admin/troeIndexes, kept in the default
// tenant's database: the table troe_db_indexes).
//
// A declaration is two partial indexes of troe_attrs in the database of every tenant it covers:
//
//   cor_n_<id>   (v_number) WHERE attr_name = '<attr>' [AND entity_type && ARRAY['<type>']]
//   cor_t_<id>   (v_text)   WHERE attr_name = '<attr>' [AND entity_type && ARRAY['<type>']]
//
// <id> is the declaration's: 8 hex digits of a CRC-32C of its attribute, type and tenant. An index of this
// naming that no declaration names is dropped at a tenant's setup.
//
// Discovered: the indexes of troe_attrs that serve a q value term, whoever made them - the declared ones
// that are there, and ones made in the database: a partial index on v_number or v_text of one attribute
// (as above), or an index on (attr_name, v_number) / (attr_name, v_text). Read at a pool's build, at every
// /admin/troeIndexes and /admin/metrics, and by a query when 60 s old: an index dropped in the database is
// not taken for one that is there, an index made there is used.
//
// A query whose q has a term an index serves takes its entities from the index (timescaleQuery.c,
// troeQTreeToCandidateSql) - only then: without the index, that form reads more than the walk.
//
#include <stdbool.h>                                 // bool
#include <stdint.h>                                  // uint32_t, int64_t
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // free, calloc, strtoll
#include <string.h>                                  // strcmp, strdup, strncmp, strstr
#include <time.h>                                    // time
#include <pthread.h>                                 // pthread_mutex_t

#include <libpq-fe.h>                                // PG*

#include "corBase/corCrc32c.h"                       // corCrc32c
#include "corBase/corStringSplit.h"                  // corStringSplit
#include "corAlloc/CorAlloc.h"                       // CorAlloc
#include "corAlloc/corAlloc.h"                       // corAlloc
#include "corAlloc/corAllocBufferInit.h"             // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"            // corAllocBufferReset
#include "corAlloc/corAllocStrdup.h"                 // corAllocStrdup
#include "corLog/corLog.h"                           // COR_I, COR_E, COR_W
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeBuilder.h"                  // corTreeArray, corTreeObject, corTreeString, corTreeInteger, corTreeChildAdd
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corJson/CorJson.h"                         // CorJson
#include "corJson/corJsonCreate.h"                   // corJsonCreate
#include "corJson/corJsonParse.h"                    // corJsonParse
#include "corJsonld/corLdInit.h"                     // corLdCoreContext
#include "corJsonld/corLdExpand.h"                   // corLdExpand
#include "corNgsild/LdQ.h"                           // LdQNode
#include "corRest/CorRestState.h"                    // corRest

#include "db/DbDriver.h"                             // DB_OK, DB_ERR, DB_NOT_FOUND, DB_ALREADY_EXISTS, DB_BAD_INPUT
#include "db/Tenant.h"                               // Tenant, tenant0, tenantList, tenantApiName
#include "troe/TroeDriver.h"                         // TroeQueryFilter
#include "troe/troeQTreeToSql.h"                     // troeQTreeToCandidateSql

#include "temporal/timescale/timescaleGlobals.h"     // timescaleDbIndexes
#include "temporal/timescale/timescalePool.h"        // TimescalePool, timescaleConnGet, timescaleConnRelease
#include "temporal/timescale/timescaleIndexes.h"     // Own interface



// -----------------------------------------------------------------------------
//
// IndexDecl - one declaration
//
typedef struct IndexDecl
{
  char   id[12];         // 8 hex digits
  char*  attr;           // expanded
  char*  type;           // expanded - NULL: every type
  char*  tenant;         // NULL: every tenant
  bool   api;            // made through /admin/troeIndexes (kept in the database) - false: --troeDbIndexes
} IndexDecl;



// -----------------------------------------------------------------------------
//
// declV - the declarations, --troeDbIndexes's first. Two indexes each, and every one costs every write of
// its attribute's history
//
static IndexDecl        declV[32];
static int              declN       = 0;
static pthread_mutex_t  declMutex   = PTHREAD_MUTEX_INITIALIZER;
static const int        declMax     = sizeof(declV) / sizeof(declV[0]);



// -----------------------------------------------------------------------------
//
// Served - an index of troe_attrs that serves a q value term; TenantIndexes - a pool's, as discovered
//
typedef struct Served
{
  char*  attr;           // NULL: every attribute (an index on (attr_name, v_*))
  char*  type;           // NULL: every type
  bool   number;         // on v_number
  bool   text;           // on v_text
} Served;

typedef struct TenantIndexes
{
  pthread_mutex_t  mutex;
  Served*          servedV;
  int              servedN;
  time_t           at;               // when discovered
} TenantIndexes;



// -----------------------------------------------------------------------------
//
// execSimple - an SQL statement without a result: false on an error, logged
//
static bool execSimple(PGconn* conn, const char* sql)
{
  PGresult* res = PQexec(conn, sql);
  bool      ok  = (PQresultStatus(res) == PGRES_COMMAND_OK) || (PQresultStatus(res) == PGRES_TUPLES_OK);

  if (ok == false)
    COR_E("timescale: db '%s': %s: %s", PQdb(conn), sql, PQerrorMessage(conn));

  PQclear(res);
  return ok;
}



// -----------------------------------------------------------------------------
//
// sqlLit - s as the body of an SQL string literal (a quote doubled), in buf
//
static const char* sqlLit(const char* s, char* buf, int bufSize)
{
  int n = 0;

  for (const char* cP = s; (*cP != 0) && (n < bufSize - 2); cP++)
  {
    if (*cP == '\'')
      buf[n++] = '\'';
    buf[n++] = *cP;
  }
  buf[n] = 0;

  return buf;
}



// -----------------------------------------------------------------------------
//
// declIdSet - the id of a declaration: its attribute, type and tenant
//
static void declIdSet(IndexDecl* dP)
{
  uint32_t crc = corCrc32c(0, dP->attr, strlen(dP->attr) + 1);

  crc = corCrc32c(crc, (dP->type   != NULL)? dP->type   : "", (dP->type   != NULL)? strlen(dP->type)   + 1 : 1);
  crc = corCrc32c(crc, (dP->tenant != NULL)? dP->tenant : "", (dP->tenant != NULL)? strlen(dP->tenant) + 1 : 1);

  snprintf(dP->id, sizeof(dP->id), "%08x", crc);
}



// -----------------------------------------------------------------------------
//
// declLookup - the declaration of an id (declMutex held) - -1: none
//
static int declLookup(const char* id)
{
  for (int ix = 0; ix < declN; ix++)
  {
    if (strcmp(declV[ix].id, id) == 0)
      return ix;
  }

  return -1;
}



// -----------------------------------------------------------------------------
//
// declFree -
//
static void declFree(IndexDecl* dP)
{
  free(dP->attr);
  free(dP->type);
  free(dP->tenant);
}



// -----------------------------------------------------------------------------
//
// declAdd - a declaration appended (declMutex held): its strings copied. DB_OK, DB_ALREADY_EXISTS (the
// same attribute, type and tenant), DB_BAD_INPUT (more than declMax) - errorText says which
//
static int declAdd(const char* attr, const char* type, const char* tenant, bool api, char* errorText, int errorTextLen)
{
  IndexDecl d = { "", strdup(attr), (type != NULL)? strdup(type) : NULL, (tenant != NULL)? strdup(tenant) : NULL, api };

  declIdSet(&d);

  int ix = declLookup(d.id);
  if (ix >= 0)
  {
    snprintf(errorText, errorTextLen, "'%s' is declared already (%s, by %s)", attr, d.id, declV[ix].api? "the admin API" : "--troeDbIndexes");
    declFree(&d);
    return DB_ALREADY_EXISTS;
  }

  if (declN >= declMax)
  {
    snprintf(errorText, errorTextLen, "%d declared indexes at most", declMax);
    declFree(&d);
    return DB_BAD_INPUT;
  }

  declV[declN++] = d;
  return DB_OK;
}



// -----------------------------------------------------------------------------
//
// declCovers - does a declaration cover a tenant?
//
static bool declCovers(IndexDecl* dP, Tenant* tenantP)
{
  return (dP->tenant == NULL) || (strcmp(dP->tenant, tenantApiName(tenantP)) == 0);
}



// -----------------------------------------------------------------------------
//
// indexesCreate - the two indexes of a declaration in a tenant's troe_attrs, over every row of its
// history: false on an error, said in errorText
//
static bool indexesCreate(PGconn* conn, IndexDecl* dP, char* errorText, int errorTextLen)
{
  char  attr[1024];
  char  type[1024];
  char  where[2200];

  sqlLit(dP->attr, attr, sizeof(attr));
  if (dP->type != NULL)
    snprintf(where, sizeof(where), "attr_name = '%s' AND entity_type && ARRAY['%s']", attr, sqlLit(dP->type, type, sizeof(type)));
  else
    snprintf(where, sizeof(where), "attr_name = '%s'", attr);

  for (int ix = 0; ix < 2; ix++)
  {
    char       sql[2400];
    PGresult*  res;

    snprintf(sql, sizeof(sql), "CREATE INDEX IF NOT EXISTS cor_%c_%s ON troe_attrs (%s) WHERE %s",
             (ix == 0)? 'n' : 't', dP->id, (ix == 0)? "v_number" : "v_text", where);

    res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
      snprintf(errorText, errorTextLen, "%s", PQerrorMessage(conn));
      PQclear(res);
      return false;
    }
    PQclear(res);
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// indexesDrop - the two indexes of a declaration id dropped (one not there is fine)
//
static void indexesDrop(PGconn* conn, const char* id)
{
  char sql[96];

  snprintf(sql, sizeof(sql), "DROP INDEX IF EXISTS cor_n_%s, cor_t_%s", id, id);
  execSimple(conn, sql);
}



// -----------------------------------------------------------------------------
//
// declsSave - the admin API's declarations written to the default tenant's database (declMutex held)
//
static bool declsSave(char* errorText, int errorTextLen)
{
  TimescaleConn* cP = timescaleConnGet(&tenant0);

  if (cP == NULL)
  {
    snprintf(errorText, errorTextLen, "keeping the declarations: no connection to the default tenant's database");
    return false;
  }

  bool ok = execSimple(cP->conn, "CREATE TABLE IF NOT EXISTS troe_db_indexes (attribute TEXT NOT NULL, type TEXT, tenant TEXT)") &&
            execSimple(cP->conn, "BEGIN") &&
            execSimple(cP->conn, "DELETE FROM troe_db_indexes");

  for (int ix = 0; (ok == true) && (ix < declN); ix++)
  {
    if (declV[ix].api == false)
      continue;

    const char* paramV[3] = { declV[ix].attr, declV[ix].type, declV[ix].tenant };
    PGresult*   res       = PQexecParams(cP->conn, "INSERT INTO troe_db_indexes (attribute, type, tenant) VALUES ($1, $2, $3)", 3, NULL, paramV, NULL, NULL, 0);

    ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
  }

  if (ok == true)
    ok = execSimple(cP->conn, "COMMIT");
  else
  {
    snprintf(errorText, errorTextLen, "keeping the declarations in '%s': %s", PQdb(cP->conn), PQerrorMessage(cP->conn));
    execSimple(cP->conn, "ROLLBACK");
  }

  timescaleConnRelease(cP);
  return ok;
}



// -----------------------------------------------------------------------------
//
// declsLoad - the admin API's declarations read from the default tenant's database
//
static bool declsLoad(PGconn* conn)
{
  char       errorText[256];
  PGresult*  res = PQexec(conn, "SELECT to_regclass('troe_db_indexes') IS NOT NULL");
  bool       there;

  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    COR_E("timescale: reading the declared indexes in '%s': %s", PQdb(conn), PQerrorMessage(conn));
    PQclear(res);
    return false;
  }

  there = (PQgetvalue(res, 0, 0)[0] == 't');        // made by the admin API's first declaration
  PQclear(res);

  if (there == false)
    return true;

  res = PQexec(conn, "SELECT attribute, type, tenant FROM troe_db_indexes");
  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    COR_E("timescale: reading the declared indexes in '%s': %s", PQdb(conn), PQerrorMessage(conn));
    PQclear(res);
    return false;
  }

  for (int row = 0; row < PQntuples(res); row++)
  {
    const char* attr   = PQgetvalue(res, row, 0);
    const char* type   = PQgetisnull(res, row, 1)? NULL : PQgetvalue(res, row, 1);
    const char* tenant = PQgetisnull(res, row, 2)? NULL : PQgetvalue(res, row, 2);

    if (declAdd(attr, type, tenant, true, errorText, sizeof(errorText)) == DB_BAD_INPUT)
      COR_W("timescale: the declared index of '%s' in '%s' is left out: %s", attr, PQdb(conn), errorText);
  }

  PQclear(res);
  return true;
}



// -----------------------------------------------------------------------------
//
// optionDeclare - one declaration of --troeDbIndexes: its names expanded with the core @context (a name
// with a ':' is taken as it is)
//
static bool optionDeclare(CorAlloc* allocP, const char* attr, const char* type, const char* tenant)
{
  char  errorText[256];
  char* attrIri = corLdExpand(corLdCoreContext(), attr, allocP, NULL, NULL);
  char* typeIri = (type != NULL)? corLdExpand(corLdCoreContext(), type, allocP, NULL, NULL) : NULL;

  if (declAdd((attrIri != NULL)? attrIri : attr, (typeIri != NULL)? typeIri : type, tenant, false, errorText, sizeof(errorText)) != DB_OK)
  {
    COR_E("timescale: --troeDbIndexes: %s", errorText);
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// optionParse - --troeDbIndexes: attribute names, comma-separated - or a JSON array of
// { "attribute": , "type"?: , "tenant"?: }
//
static bool optionParse(const char* option)
{
  static char  buf[4096];
  CorAlloc     alloc;
  bool         ok = true;

  corAllocBufferInit(&alloc, buf, sizeof(buf), 16 * 1024, NULL, "troeDbIndexes");

  char* text = corAllocStrdup(&alloc, option);

  while ((*text == ' ') || (*text == '\t'))
    text++;

  if (*text == '[')
  {
    CorJson  cj;

    corJsonCreate(&cj, &alloc);

    CorNode* treeP = corJsonParse(&cj, text);

    if ((treeP == NULL) || (treeP->type != CorArray))
    {
      COR_E("timescale: --troeDbIndexes: not a JSON array of { \"attribute\", \"type\", \"tenant\" }");
      ok = false;
    }

    for (CorNode* declP = (treeP != NULL)? treeP->value.head : NULL; (ok == true) && (declP != NULL); declP = declP->next)
    {
      CorNode* attrP   = (declP->type == CorObject)? corTreeLookup(declP, "attribute") : NULL;
      CorNode* typeP   = (declP->type == CorObject)? corTreeLookup(declP, "type")      : NULL;
      CorNode* tenantP = (declP->type == CorObject)? corTreeLookup(declP, "tenant")    : NULL;

      if ((attrP == NULL) || (attrP->type != CorString) || ((typeP != NULL) && (typeP->type != CorString)) || ((tenantP != NULL) && (tenantP->type != CorString)))
      {
        COR_E("timescale: --troeDbIndexes: each declaration is { \"attribute\": <string>, \"type\"?: <string>, \"tenant\"?: <string> }");
        ok = false;
      }
      else
        ok = optionDeclare(&alloc, attrP->value.s, (typeP != NULL)? typeP->value.s : NULL, (tenantP != NULL)? tenantP->value.s : NULL);
    }
  }
  else
  {
    char* nameV[64];
    int   nameN = corStringSplit(text, ',', nameV, sizeof(nameV) / sizeof(nameV[0]));

    for (int ix = 0; (ok == true) && (ix < nameN); ix++)
    {
      if (nameV[ix][0] != 0)
        ok = optionDeclare(&alloc, nameV[ix], NULL, NULL);
    }
  }

  corAllocBufferReset(&alloc, false);
  return ok;
}



// -----------------------------------------------------------------------------
//
// timescaleIndexesInit -
//
int timescaleIndexesInit(void)
{
  pthread_mutex_lock(&declMutex);

  bool ok = (timescaleDbIndexes == NULL) || (optionParse(timescaleDbIndexes) == true);

  pthread_mutex_unlock(&declMutex);

  return ok? 0 : -1;
}



// -----------------------------------------------------------------------------
//
// timescaleIndexesLoad -
//
int timescaleIndexesLoad(void)
{
  TimescaleConn* cP = timescaleConnGet(&tenant0);

  if (cP == NULL)
    return -1;

  pthread_mutex_lock(&declMutex);
  int  before = declN;
  bool ok     = declsLoad(cP->conn);
  pthread_mutex_unlock(&declMutex);

  if (declN > before)
    timescaleIndexesApply(cP->conn, &tenant0);    // the default tenant's pool was built before they were known
  timescaleIndexesDiscover(cP->conn, (TimescalePool*) tenant0.troePoolP);

  timescaleConnRelease(cP);

  if (declN > 0)
    COR_I("timescale: %d declared index(es)", declN);

  return ok? 0 : -1;
}



// -----------------------------------------------------------------------------
//
// timescaleIndexesApply -
//
void timescaleIndexesApply(PGconn* conn, Tenant* tenantP)
{
  char       errorText[512];
  PGresult*  res = PQexec(conn, "SELECT indexname FROM pg_indexes WHERE schemaname = current_schema() AND tablename = 'troe_attrs' "
                                "AND (indexname LIKE 'cor\\_n\\_%' OR indexname LIKE 'cor\\_t\\_%')");

  pthread_mutex_lock(&declMutex);

  if (PQresultStatus(res) == PGRES_TUPLES_OK)
  {
    for (int row = 0; row < PQntuples(res); row++)
    {
      const char* id = &PQgetvalue(res, row, 0)[6];
      int         ix = declLookup(id);

      if ((ix < 0) || (declCovers(&declV[ix], tenantP) == false))
      {
        COR_I("timescale: db '%s': the indexes of declaration %s dropped - it is no longer declared", PQdb(conn), id);
        indexesDrop(conn, id);
      }
    }
  }
  else
    COR_W("timescale: db '%s': listing its indexes: %s", PQdb(conn), PQerrorMessage(conn));
  PQclear(res);

  for (int ix = 0; ix < declN; ix++)
  {
    if (declCovers(&declV[ix], tenantP) && (indexesCreate(conn, &declV[ix], errorText, sizeof(errorText)) == false))
      COR_E("timescale: db '%s': the declared index of '%s' (%s): %s", PQdb(conn), declV[ix].attr, declV[ix].id, errorText);
  }

  pthread_mutex_unlock(&declMutex);
}



// -----------------------------------------------------------------------------
//
// litRead - an SQL string literal of pg_indexes's text ('...', a quote doubled) - sP at its opening quote:
// where it ends, NULL if it is not one
//
static const char* litRead(const char* sP, char* buf, int bufSize)
{
  int n = 0;

  if (*sP != '\'')
    return NULL;

  for (++sP; *sP != 0; ++sP)
  {
    if (*sP == '\'')
    {
      if (sP[1] != '\'')
        break;
      ++sP;
    }

    if (n >= bufSize - 1)
      return NULL;
    buf[n++] = *sP;
  }

  buf[n] = 0;
  return (*sP == '\'')? sP + 1 : NULL;
}



// -----------------------------------------------------------------------------
//
// servedOf - what an index of troe_attrs serves, by its definition (pg_indexes.indexdef): false if no
// q value term. The forms:
//
//   ... USING btree (v_number) WHERE (attr_name = '<attr>'::text)
//   ... USING btree (v_number) WHERE ((attr_name = '<attr>'::text) AND (entity_type && ARRAY['<type>'::text]))
//   ... USING btree (attr_name, v_number[, ...])
//
// v_text as v_number
//
static bool servedOf(const char* def, Served* sP)
{
  char        attr[1024];
  char        type[1024];
  const char* cP = strstr(def, " USING btree (");

  memset(sP, 0, sizeof(Served));

  if (cP == NULL)
    return false;
  cP += 14;

  if (strncmp(cP, "attr_name, ", 11) == 0)
  {
    cP += 11;
    if      ((strncmp(cP, "v_number", 8) == 0) && ((cP[8] == ')') || (cP[8] == ','))) sP->number = true;
    else if ((strncmp(cP, "v_text",   6) == 0) && ((cP[6] == ')') || (cP[6] == ','))) sP->text   = true;
    else return false;

    return (strstr(cP, " WHERE ") == NULL);
  }

  if      (strncmp(cP, "v_number) WHERE ", 16) == 0) { sP->number = true; cP += 16; }
  else if (strncmp(cP, "v_text) WHERE ",   14) == 0) { sP->text   = true; cP += 14; }
  else return false;

  bool two = (strncmp(cP, "((attr_name = ", 14) == 0);

  if      (two == true)                              cP += 14;
  else if (strncmp(cP, "(attr_name = ", 13) == 0)   cP += 13;
  else return false;

  if (((cP = litRead(cP, attr, sizeof(attr))) == NULL) || (strncmp(cP, "::text)", 7) != 0))
    return false;
  cP += 7;

  if (two == true)
  {
    if (strncmp(cP, " AND (entity_type && ARRAY[", 27) != 0)
      return false;
    cP += 27;
    if (((cP = litRead(cP, type, sizeof(type))) == NULL) || (strcmp(cP, "::text]))") != 0))
      return false;
    sP->type = strdup(type);
  }
  else if (*cP != 0)
    return false;

  sP->attr = strdup(attr);
  return true;
}



// -----------------------------------------------------------------------------
//
// servedFree -
//
static void servedFree(Served* servedV, int servedN)
{
  for (int ix = 0; ix < servedN; ix++)
  {
    free(servedV[ix].attr);
    free(servedV[ix].type);
  }
  free(servedV);
}



// -----------------------------------------------------------------------------
//
// timescaleIndexesDiscover -
//
void timescaleIndexesDiscover(PGconn* conn, TimescalePool* poolP)
{
  if (poolP == NULL)
    return;

  PGresult* res = PQexec(conn, "SELECT indexdef FROM pg_indexes WHERE schemaname = current_schema() AND tablename = 'troe_attrs'");

  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    COR_W("timescale: db '%s': listing its indexes: %s", PQdb(conn), PQerrorMessage(conn));
    PQclear(res);
    return;
  }

  int      rows    = PQntuples(res);
  Served*  servedV = (Served*) calloc((rows > 0)? rows : 1, sizeof(Served));
  int      servedN = 0;

  for (int row = 0; (servedV != NULL) && (row < rows); row++)
  {
    if (servedOf(PQgetvalue(res, row, 0), &servedV[servedN]) == true)
      ++servedN;
  }
  PQclear(res);

  if (servedV == NULL)
    return;

  TenantIndexes* tiP = (TenantIndexes*) poolP->indexesP;

  if (tiP == NULL)
  {
    tiP = (TenantIndexes*) calloc(1, sizeof(TenantIndexes));
    if (tiP == NULL)
    {
      servedFree(servedV, servedN);
      return;
    }
    pthread_mutex_init(&tiP->mutex, NULL);
    poolP->indexesP = tiP;
  }

  pthread_mutex_lock(&tiP->mutex);
  servedFree(tiP->servedV, tiP->servedN);
  tiP->servedV = servedV;
  tiP->servedN = servedN;
  tiP->at      = time(NULL);
  pthread_mutex_unlock(&tiP->mutex);
}



// -----------------------------------------------------------------------------
//
// timescaleIndexesPoolFree -
//
void timescaleIndexesPoolFree(TimescalePool* poolP)
{
  TenantIndexes* tiP = (TenantIndexes*) poolP->indexesP;

  if (tiP == NULL)
    return;

  servedFree(tiP->servedV, tiP->servedN);
  pthread_mutex_destroy(&tiP->mutex);
  free(tiP);
  poolP->indexesP = NULL;
}



// -----------------------------------------------------------------------------
//
// ServedWork - a query's: the indexes of its tenant (a copy) and its one entity type, if it has one
//
typedef struct ServedWork
{
  Served*      servedV;
  int          servedN;
  const char*  type;              // the query's, if it names exactly one
  CorAlloc*    allocP;
} ServedWork;



// -----------------------------------------------------------------------------
//
// servedBy - TroeServedFunc: an index of the attribute in the column, for every type or the query's one
//
static const char* servedBy(const char* attr, const char* column, void* dataP)
{
  ServedWork* wP     = (ServedWork*) dataP;
  bool        number = (strcmp(column, "v_number") == 0);

  for (int ix = 0; ix < wP->servedN; ix++)
  {
    Served* sP = &wP->servedV[ix];

    if (((number == true) && (sP->number == false)) || ((number == false) && (sP->text == false)))
      continue;
    if ((sP->attr != NULL) && (strcmp(sP->attr, attr) != 0))
      continue;
    if (sP->type == NULL)
      return "";
  }

  //
  // An index narrowed to a type: its condition in the subquery too, for the index to be the planner's
  //
  for (int ix = 0; ix < wP->servedN; ix++)
  {
    Served* sP = &wP->servedV[ix];

    if ((sP->type == NULL) || (wP->type == NULL) || (strcmp(sP->type, wP->type) != 0))
      continue;
    if (((number == true) && (sP->number == false)) || ((number == false) && (sP->text == false)))
      continue;
    if ((sP->attr != NULL) && (strcmp(sP->attr, attr) != 0))
      continue;

    char   lit[1024];
    int    sz    = (int) strlen(sP->type) * 2 + 48;
    char*  extra = (char*) corAlloc(wP->allocP, sz);

    snprintf(extra, sz, " AND entity_type && ARRAY['%s']", sqlLit(sP->type, lit, sizeof(lit)));
    return extra;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// timescaleIndexesCandidates -
//
const char* timescaleIndexesCandidates(PGconn* conn, Tenant* tenantP, TroeQueryFilter* fP, CorAlloc* allocP)
{
  TimescalePool*  poolP = (TimescalePool*) tenantP->troePoolP;
  TenantIndexes*  tiP   = (poolP != NULL)? (TenantIndexes*) poolP->indexesP : NULL;

  if ((fP->qTree == NULL) || (tiP == NULL))
    return NULL;

  if (time(NULL) - tiP->at >= 60)                    // an index made or dropped in the database meanwhile
    timescaleIndexesDiscover(conn, poolP);

  ServedWork w = { NULL, 0, NULL, allocP };

  if ((fP->typeV != NULL) && (fP->typeV[0] != NULL) && (fP->typeV[1] == NULL))
    w.type = fP->typeV[0];

  pthread_mutex_lock(&tiP->mutex);

  if (tiP->servedN > 0)
  {
    w.servedV = (Served*) corAlloc(allocP, tiP->servedN * sizeof(Served));
    for (int ix = 0; ix < tiP->servedN; ix++)
    {
      w.servedV[ix]      = tiP->servedV[ix];
      w.servedV[ix].attr = (tiP->servedV[ix].attr != NULL)? corAllocStrdup(allocP, tiP->servedV[ix].attr) : NULL;
      w.servedV[ix].type = (tiP->servedV[ix].type != NULL)? corAllocStrdup(allocP, tiP->servedV[ix].type) : NULL;
    }
    w.servedN = tiP->servedN;
  }

  pthread_mutex_unlock(&tiP->mutex);

  if (w.servedN == 0)
    return NULL;

  return troeQTreeToCandidateSql((LdQNode*) fP->qTree, servedBy, &w, allocP);
}



// -----------------------------------------------------------------------------
//
// IndexStats / tenantStats - every index of a tenant's troe_attrs: its definition, and its bytes and scans -
// a hypertable's chunks' indexes added up (TimescaleDB names them _hyper_<n>_<m>_chunk_<index>)
//
typedef struct IndexStats
{
  char*    name;
  char*    def;
  int64_t  bytes;
  int64_t  scans;
} IndexStats;

static int tenantStats(PGconn* conn, IndexStats** vPP)
{
  PGresult* res = PQexec(conn,
                         "SELECT i.indexname, i.indexdef, "
                         "  coalesce(sum(pg_relation_size(s.indexrelid)), 0), coalesce(sum(s.idx_scan), 0) "
                         "FROM pg_indexes i LEFT JOIN pg_stat_user_indexes s "
                         "  ON s.indexrelname = i.indexname OR s.indexrelname ~ ('^_hyper_[0-9]+_[0-9]+_chunk_' || i.indexname || '$') "
                         "WHERE i.schemaname = current_schema() AND i.tablename = 'troe_attrs' "
                         "GROUP BY i.indexname, i.indexdef ORDER BY i.indexname");

  *vPP = NULL;

  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    COR_W("timescale: db '%s': the indexes' figures: %s", PQdb(conn), PQerrorMessage(conn));
    PQclear(res);
    return 0;
  }

  int          rows = PQntuples(res);
  IndexStats*  vP   = (IndexStats*) corAlloc(corRest.kallocP, ((rows > 0)? rows : 1) * sizeof(IndexStats));

  for (int row = 0; row < rows; row++)
  {
    vP[row].name  = corAllocStrdup(corRest.kallocP, PQgetvalue(res, row, 0));
    vP[row].def   = corAllocStrdup(corRest.kallocP, PQgetvalue(res, row, 1));
    vP[row].bytes = strtoll(PQgetvalue(res, row, 2), NULL, 10);
    vP[row].scans = strtoll(PQgetvalue(res, row, 3), NULL, 10);
  }

  PQclear(res);
  *vPP = vP;
  return rows;
}



// -----------------------------------------------------------------------------
//
// TenantSeen - a tenant's indexes, read once for a GET
//
typedef struct TenantSeen
{
  Tenant*      tenantP;
  IndexStats*  statsV;
  int          statsN;
} TenantSeen;



// -----------------------------------------------------------------------------
//
// tenantsSeen - every tenant whose history is open, the default one first, with its indexes (and their
// discovery refreshed)
//
static int tenantsSeen(TenantSeen** vPP)
{
  int n = 0;

  for (Tenant* tP = &tenant0; tP != NULL; tP = (tP == &tenant0)? tenantList : tP->next)
  {
    if (tP->troePoolP != NULL)
      ++n;
  }

  TenantSeen* vP = (TenantSeen*) corAlloc(corRest.kallocP, ((n > 0)? n : 1) * sizeof(TenantSeen));
  int         ix = 0;

  for (Tenant* tP = &tenant0; (tP != NULL) && (ix < n); tP = (tP == &tenant0)? tenantList : tP->next)
  {
    if (tP->troePoolP == NULL)
      continue;

    TimescaleConn* cP = timescaleConnGet(tP);

    vP[ix].tenantP = tP;
    vP[ix].statsV  = NULL;
    vP[ix].statsN  = 0;

    if (cP != NULL)
    {
      vP[ix].statsN = tenantStats(cP->conn, &vP[ix].statsV);
      timescaleIndexesDiscover(cP->conn, (TimescalePool*) tP->troePoolP);
      timescaleConnRelease(cP);
    }
    ++ix;
  }

  *vPP = vP;
  return ix;
}



// -----------------------------------------------------------------------------
//
// tenantNode - a tenant's figures of an index (or a declaration's two)
//
static CorNode* tenantNode(Tenant* tenantP, bool built, int64_t bytes, int64_t accesses)
{
  CorNode* tP = corTreeObject(corRest.kallocP, NULL);

  corTreeChildAdd(tP, corTreeString(corRest.kallocP, "tenant", tenantApiName(tenantP)));
  corTreeChildAdd(tP, corTreeBoolean(corRest.kallocP, "built", built));
  corTreeChildAdd(tP, corTreeInteger(corRest.kallocP, "bytes", bytes));
  corTreeChildAdd(tP, corTreeInteger(corRest.kallocP, "accesses", accesses));

  return tP;
}



// -----------------------------------------------------------------------------
//
// declToTree - a declaration as the admin API shows it (declMutex held); with seenV: its tenants with
// stats
//
static CorNode* declToTree(IndexDecl* dP, TenantSeen* seenV, int seenN)
{
  CorNode* declP = corTreeObject(corRest.kallocP, NULL);

  corTreeChildAdd(declP, corTreeString(corRest.kallocP, "id", dP->id));
  corTreeChildAdd(declP, corTreeString(corRest.kallocP, "attribute", dP->attr));
  if (dP->type != NULL)
    corTreeChildAdd(declP, corTreeString(corRest.kallocP, "type", dP->type));
  if (dP->tenant != NULL)
    corTreeChildAdd(declP, corTreeString(corRest.kallocP, "tenant", dP->tenant));
  corTreeChildAdd(declP, corTreeString(corRest.kallocP, "declaredBy", dP->api? "api" : "troeDbIndexes"));

  if (seenV == NULL)
    return declP;

  CorNode* tenantsP = corTreeArray(corRest.kallocP, "tenants");

  for (int ix = 0; ix < seenN; ix++)
  {
    TenantSeen* sP       = &seenV[ix];
    int         found    = 0;
    int64_t     bytes    = 0;
    int64_t     accesses = 0;

    if (declCovers(dP, sP->tenantP) == false)
      continue;

    for (int jx = 0; jx < sP->statsN; jx++)
    {
      const char* name = sP->statsV[jx].name;

      if ((strncmp(name, "cor_", 4) == 0) && ((name[4] == 'n') || (name[4] == 't')) && (name[5] == '_') && (strcmp(&name[6], dP->id) == 0))
      {
        ++found;
        bytes    += sP->statsV[jx].bytes;
        accesses += sP->statsV[jx].scans;
      }
    }

    corTreeChildAdd(tenantsP, tenantNode(sP->tenantP, found == 2, bytes, accesses));
  }

  corTreeChildAdd(declP, tenantsP);
  return declP;
}



// -----------------------------------------------------------------------------
//
// timescaleIndexList - the declarations, then the indexes made in a tenant's database that serve a q value
// term (declaredBy "database", id = the index's name)
//
int timescaleIndexList(CorNode** arrayPP)
{
  CorNode*     arrayP = corTreeArray(corRest.kallocP, NULL);
  TenantSeen*  seenV;
  int          seenN  = tenantsSeen(&seenV);

  pthread_mutex_lock(&declMutex);

  for (int ix = 0; ix < declN; ix++)
    corTreeChildAdd(arrayP, declToTree(&declV[ix], seenV, seenN));

  pthread_mutex_unlock(&declMutex);

  for (int ix = 0; ix < seenN; ix++)
  {
    for (int jx = 0; jx < seenV[ix].statsN; jx++)
    {
      IndexStats*  iP = &seenV[ix].statsV[jx];
      Served       s;

      if ((strncmp(iP->name, "cor_n_", 6) == 0) || (strncmp(iP->name, "cor_t_", 6) == 0))
        continue;
      if (servedOf(iP->def, &s) == false)
        continue;

      CorNode* declP    = corTreeObject(corRest.kallocP, NULL);
      CorNode* tenantsP = corTreeArray(corRest.kallocP, "tenants");

      corTreeChildAdd(declP, corTreeString(corRest.kallocP, "id", iP->name));
      if (s.attr != NULL)
        corTreeChildAdd(declP, corTreeString(corRest.kallocP, "attribute", s.attr));
      if (s.type != NULL)
        corTreeChildAdd(declP, corTreeString(corRest.kallocP, "type", s.type));
      corTreeChildAdd(declP, corTreeString(corRest.kallocP, "tenant", tenantApiName(seenV[ix].tenantP)));
      corTreeChildAdd(declP, corTreeString(corRest.kallocP, "declaredBy", "database"));
      corTreeChildAdd(declP, corTreeString(corRest.kallocP, "definition", iP->def));
      corTreeChildAdd(tenantsP, tenantNode(seenV[ix].tenantP, true, iP->bytes, iP->scans));
      corTreeChildAdd(declP, tenantsP);
      corTreeChildAdd(arrayP, declP);

      free(s.attr);
      free(s.type);
    }
  }

  *arrayPP = arrayP;
  return DB_OK;
}



// -----------------------------------------------------------------------------
//
// tenantsEachConn - every tenant whose history is open and the declaration covers: f on a connection of its
// pool - false stops
//
typedef bool (*TenantConnFunc)(Tenant* tenantP, PGconn* conn, IndexDecl* dP, char* errorText, int errorTextLen);

static bool tenantsEachConn(TenantConnFunc f, IndexDecl* dP, char* errorText, int errorTextLen)
{
  for (Tenant* tP = &tenant0; tP != NULL; tP = (tP == &tenant0)? tenantList : tP->next)
  {
    if ((tP->troePoolP == NULL) || (declCovers(dP, tP) == false))
      continue;

    TimescaleConn* cP = timescaleConnGet(tP);

    if (cP == NULL)
    {
      snprintf(errorText, errorTextLen, "no connection to the history of tenant '%s'", tenantApiName(tP));
      return false;
    }

    bool go = f(tP, cP->conn, dP, errorText, errorTextLen);

    timescaleIndexesDiscover(cP->conn, (TimescalePool*) tP->troePoolP);
    timescaleConnRelease(cP);

    if (go == false)
      return false;
  }

  return true;
}



static bool createIn(Tenant* tenantP, PGconn* conn, IndexDecl* dP, char* errorText, int errorTextLen)
{
  if (indexesCreate(conn, dP, errorText, errorTextLen) == false)
  {
    COR_E("timescale: db '%s': the declared index of '%s' (%s): %s", PQdb(conn), dP->attr, dP->id, errorText);
    return false;
  }

  COR_I("timescale: db '%s': the declared index of '%s' (%s) created", PQdb(conn), dP->attr, dP->id);
  return true;
}



static bool dropIn(Tenant* tenantP, PGconn* conn, IndexDecl* dP, char* errorText, int errorTextLen)
{
  indexesDrop(conn, dP->id);
  return true;
}



// -----------------------------------------------------------------------------
//
// timescaleIndexAdd - a declaration of the admin API: its indexes created in every tenant it covers, then
// kept. An index that cannot be created: none of them kept, the declaration not made (DB_ERR)
//
int timescaleIndexAdd(const char* attr, const char* type, const char* tenant, CorNode** declPP, char* errorText, int errorTextLen)
{
  pthread_mutex_lock(&declMutex);

  int rc = declAdd(attr, type, tenant, true, errorText, errorTextLen);

  if (rc == DB_OK)
  {
    IndexDecl* dP = &declV[declN - 1];

    if ((tenantsEachConn(createIn, dP, errorText, errorTextLen) == false) || (declsSave(errorText, errorTextLen) == false))
    {
      char ignored[256];

      tenantsEachConn(dropIn, dP, ignored, sizeof(ignored));
      declFree(&declV[--declN]);
      rc = DB_ERR;
    }
    else
      *declPP = declToTree(dP, NULL, 0);
  }

  pthread_mutex_unlock(&declMutex);
  return rc;
}



// -----------------------------------------------------------------------------
//
// timescaleIndexDelete - a declaration of the admin API taken away: its indexes dropped in every tenant.
// One of --troeDbIndexes is not (DB_BAD_INPUT): it is the start's, and comes back with it
//
int timescaleIndexDelete(const char* id, char* errorText, int errorTextLen)
{
  int rc = DB_OK;

  pthread_mutex_lock(&declMutex);

  int ix = declLookup(id);

  if (ix < 0)
  {
    snprintf(errorText, errorTextLen, "no declared index '%s'", id);
    rc = DB_NOT_FOUND;
  }
  else if (declV[ix].api == false)
  {
    snprintf(errorText, errorTextLen, "'%s' is declared by --troeDbIndexes - taken out of it, it is gone at the next start", id);
    rc = DB_BAD_INPUT;
  }
  else
  {
    IndexDecl d = declV[ix];

    for (int jx = ix; jx < declN - 1; jx++)
      declV[jx] = declV[jx + 1];
    declN--;

    if (declsSave(errorText, errorTextLen) == false)
    {
      for (int jx = declN; jx > ix; jx--)
        declV[jx] = declV[jx - 1];
      declV[ix] = d;
      declN++;
      rc = DB_ERR;
    }
    else
    {
      tenantsEachConn(dropIn, &d, errorText, errorTextLen);
      declFree(&d);
    }
  }

  pthread_mutex_unlock(&declMutex);
  return rc;
}
