//
// FILE            mongocIndexes.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The declared indexes (doc/installation.md, "Declared indexes"): an attribute - narrowed to an entity
// type, to a tenant, if so declared - whose q value terms an index serves. Declared at start
// (--dbValueIndexes) or through the admin API (/admin/indexes, kept in the default tenant's database:
// <db>.metadata { _id: "dbIndexes", declarations: [ { attribute, type?, tenant? } ] }).
//
// A declaration is two indexes in the entities collection of every tenant it covers - the q pre-filter's
// two halves (mongocEntityQuery.c, bsonAppendInstancePrefilter):
//
//   cor_v_<id>   { <attr>.@none.value: 1 }    the default instance's value
//   cor_d_<id>   { <attr>.@datasetIds: 1 }    the attributes with an instance of a datasetId - partial:
//                                             only those, a handful
//
// the first partial on { type: <type> } when a type is declared, and a query of that one type uses it.
// Not the second: MongoDB uses neither of the two for the $or of a query when the second's partial filter
// names the type too (8.2: a query of 20 000 entities read them all through the type index) - and it
// holds the few attributes with an instance of a datasetId whatever their entity's type. <id> is the declaration's: 8 hex digits of
// a CRC-32C of its attribute, type and tenant. An index of this plugin's naming (cor_v_*, cor_d_*) that
// no declaration names is dropped at a tenant's setup: a declaration taken out of --dbValueIndexes is gone at
// the next start.
//
#include <stdbool.h>                                 // bool
#include <stdint.h>                                  // uint32_t, int64_t
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // free
#include <string.h>                                  // strcmp, strdup, strncmp
#include <pthread.h>                                 // pthread_mutex_t

#include <mongoc/mongoc.h>                           // mongoc_*, bson_*

#include "corBase/corCrc32c.h"                       // corCrc32c
#include "corBase/corStringSplit.h"                  // corStringSplit
#include "corAlloc/CorAlloc.h"                       // CorAlloc
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
#include "corRest/CorRestState.h"                    // corRest

#include "db/DbDriver.h"                             // DB_OK, DB_ERR, DB_NOT_FOUND, DB_ALREADY_EXISTS, DB_BAD_INPUT
#include "db/Tenant.h"                               // Tenant, tenant0, tenantList, tenantApiName
#include "currentState/mongoc/mongocGlobals.h"       // mongocDbIndexes, mongocDbName
#include "currentState/mongoc/mongocDotEscape.h"     // mongocEscapeDotsInKey
#include "currentState/mongoc/mongocIndexes.h"       // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



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
  bool   api;            // made through /admin/indexes (kept in the database) - false: --dbValueIndexes
} IndexDecl;



// -----------------------------------------------------------------------------
//
// declV - the declarations, --dbValueIndexes's first. 24 at most: two indexes each, and MongoDB's limit is 64
// indexes per collection, the broker's own and the geo ones among them
//
static IndexDecl        declV[24];
static int              declN       = 0;
static pthread_mutex_t  declMutex   = PTHREAD_MUTEX_INITIALIZER;
static const int        declMax     = sizeof(declV) / sizeof(declV[0]);



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
    snprintf(errorText, errorTextLen, "'%s' is declared already (%s, by %s)", attr, d.id, declV[ix].api? "the admin API" : "--dbValueIndexes");
    declFree(&d);
    return DB_ALREADY_EXISTS;
  }

  if (declN >= declMax)
  {
    snprintf(errorText, errorTextLen, "%d declared indexes at most (two indexes each - MongoDB's limit is 64 per collection)", declMax);
    declFree(&d);
    return DB_BAD_INPUT;
  }

  declV[declN++] = d;
  return DB_OK;
}



// -----------------------------------------------------------------------------
//
// declCovers - does a declaration cover a tenant? A Snapshot's own tenant is the tenant it was taken on
//
static bool declCovers(IndexDecl* dP, Tenant* tenantP)
{
  return (dP->tenant == NULL) || (strcmp(dP->tenant, tenantApiName(tenantP)) == 0);
}



// -----------------------------------------------------------------------------
//
// indexesCreate - the two indexes of a declaration in a tenant's entities collection: false on an error,
// said in errorText
//
static bool indexesCreate(mongoc_collection_t* collP, IndexDecl* dP, char* errorText, int errorTextLen)
{
  char  attrPath[1024];
  char  path[1100];
  char  name[32];

  snprintf(attrPath, sizeof(attrPath), "%s", mongocEscapeDotsInKey(dP->attr));

  bson_t valueKeys, markKeys, valueOpts, markOpts, filter;

  bson_init(&valueKeys);
  snprintf(path, sizeof(path), "%s.@none.value", attrPath);
  BSON_APPEND_INT32(&valueKeys, path, 1);

  bson_init(&valueOpts);
  snprintf(name, sizeof(name), "cor_v_%s", dP->id);
  BSON_APPEND_UTF8(&valueOpts, "name", name);
  if (dP->type != NULL)
  {
    BSON_APPEND_DOCUMENT_BEGIN(&valueOpts, "partialFilterExpression", &filter);
    BSON_APPEND_UTF8(&filter, "type", dP->type);
    bson_append_document_end(&valueOpts, &filter);
  }

  bson_init(&markKeys);
  snprintf(path, sizeof(path), "%s.@datasetIds", attrPath);
  BSON_APPEND_INT32(&markKeys, path, 1);

  bson_init(&markOpts);
  snprintf(name, sizeof(name), "cor_d_%s", dP->id);
  BSON_APPEND_UTF8(&markOpts, "name", name);
  BSON_APPEND_DOCUMENT_BEGIN(&markOpts, "partialFilterExpression", &filter);
  BSON_APPEND_BOOL(&filter, path, true);
  bson_append_document_end(&markOpts, &filter);

  mongoc_index_model_t* modelV[2] = { mongoc_index_model_new(&valueKeys, &valueOpts), mongoc_index_model_new(&markKeys, &markOpts) };
  bson_error_t          error;
  bool                  ok      = mongoc_collection_create_indexes_with_opts(collP, modelV, 2, NULL, NULL, &error);

  if (ok == false)
    snprintf(errorText, errorTextLen, "%s", error.message);

  mongoc_index_model_destroy(modelV[0]);
  mongoc_index_model_destroy(modelV[1]);
  bson_destroy(&valueKeys);
  bson_destroy(&valueOpts);
  bson_destroy(&markKeys);
  bson_destroy(&markOpts);

  return ok;
}



// -----------------------------------------------------------------------------
//
// indexesDrop - the two indexes of a declaration id dropped (one not there is fine)
//
static void indexesDrop(mongoc_collection_t* collP, const char* id, const char* dbName)
{
  char          name[32];
  bson_error_t  error;

  for (int ix = 0; ix < 2; ix++)
  {
    snprintf(name, sizeof(name), "cor_%c_%s", (ix == 0)? 'v' : 'd', id);

    if ((mongoc_collection_drop_index_with_opts(collP, name, NULL, &error) == false) && (error.code != 27))   // 27: IndexNotFound
      COR_W("mongoc: db '%s': dropping the declared index %s: %s", dbName, name, error.message);
  }
}



// -----------------------------------------------------------------------------
//
// declsSave - the admin API's declarations written to the default tenant's database (declMutex held)
//
static bool declsSave(char* errorText, int errorTextLen)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenant0.dbName, "metadata");
  bson_t                selector, update, set, array, elem, opts;
  bson_error_t          error;
  int                   aix     = 0;

  bson_init(&selector);
  BSON_APPEND_UTF8(&selector, "_id", "dbIndexes");

  bson_init(&update);
  BSON_APPEND_DOCUMENT_BEGIN(&update, "$set", &set);
  BSON_APPEND_ARRAY_BEGIN(&set, "declarations", &array);

  for (int ix = 0; ix < declN; ix++)
  {
    char key[16];

    if (declV[ix].api == false)
      continue;

    snprintf(key, sizeof(key), "%d", aix++);
    BSON_APPEND_DOCUMENT_BEGIN(&array, key, &elem);
    BSON_APPEND_UTF8(&elem, "attribute", declV[ix].attr);
    if (declV[ix].type != NULL)
      BSON_APPEND_UTF8(&elem, "type", declV[ix].type);
    if (declV[ix].tenant != NULL)
      BSON_APPEND_UTF8(&elem, "tenant", declV[ix].tenant);
    bson_append_document_end(&array, &elem);
  }

  bson_append_array_end(&set, &array);
  bson_append_document_end(&update, &set);

  bson_init(&opts);
  BSON_APPEND_BOOL(&opts, "upsert", true);

  bool ok = mongoc_collection_update_one(collP, &selector, &update, &opts, NULL, &error);

  if (ok == false)
    snprintf(errorText, errorTextLen, "keeping the declarations in '%s': %s", tenant0.dbName, error.message);

  bson_destroy(&opts);
  bson_destroy(&update);
  bson_destroy(&selector);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return ok;
}



// -----------------------------------------------------------------------------
//
// declsLoad - the admin API's declarations read from the default tenant's database
//
static bool declsLoad(void)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenant0.dbName, "metadata");
  bson_t                filter;
  const bson_t*         docP;
  bson_error_t          error;
  bool                  ok      = true;
  char                  errorText[256];

  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", "dbIndexes");

  mongoc_cursor_t* cursorP = mongoc_collection_find_with_opts(collP, &filter, NULL, NULL);

  if (mongoc_cursor_next(cursorP, &docP))
  {
    bson_iter_t  iter;
    bson_iter_t  arrayIter;

    if (bson_iter_init_find(&iter, docP, "declarations") && BSON_ITER_HOLDS_ARRAY(&iter) && bson_iter_recurse(&iter, &arrayIter))
    {
      while (bson_iter_next(&arrayIter))
      {
        bson_iter_t  declIter;
        const char*  attr   = NULL;
        const char*  type   = NULL;
        const char*  tenant = NULL;

        if ((BSON_ITER_HOLDS_DOCUMENT(&arrayIter) == false) || (bson_iter_recurse(&arrayIter, &declIter) == false))
          continue;

        while (bson_iter_next(&declIter))
        {
          const char* key = bson_iter_key(&declIter);

          if (BSON_ITER_HOLDS_UTF8(&declIter) == false)
            continue;

          if      (strcmp(key, "attribute") == 0) attr   = bson_iter_utf8(&declIter, NULL);
          else if (strcmp(key, "type")      == 0) type   = bson_iter_utf8(&declIter, NULL);
          else if (strcmp(key, "tenant")    == 0) tenant = bson_iter_utf8(&declIter, NULL);
        }

        if ((attr != NULL) && (declAdd(attr, type, tenant, true, errorText, sizeof(errorText)) == DB_BAD_INPUT))
          COR_W("mongoc: the declared index of '%s' in '%s' is left out: %s", attr, tenant0.dbName, errorText);
      }
    }
  }

  if (mongoc_cursor_error(cursorP, &error))
  {
    COR_E("mongoc: reading the declared indexes in '%s': %s", tenant0.dbName, error.message);
    ok = false;
  }

  mongoc_cursor_destroy(cursorP);
  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return ok;
}



// -----------------------------------------------------------------------------
//
// optionDeclare - one declaration of --dbValueIndexes: its names expanded with the core @context (a name
// with a ':' is taken as it is)
//
static bool optionDeclare(CorAlloc* allocP, const char* attr, const char* type, const char* tenant)
{
  char  errorText[256];
  char* attrIri = corLdExpand(corLdCoreContext(), attr, allocP, NULL, NULL);
  char* typeIri = (type != NULL)? corLdExpand(corLdCoreContext(), type, allocP, NULL, NULL) : NULL;

  if (declAdd((attrIri != NULL)? attrIri : attr, (typeIri != NULL)? typeIri : type, tenant, false, errorText, sizeof(errorText)) != DB_OK)
  {
    COR_E("mongoc: --dbValueIndexes: %s", errorText);
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// optionParse - --dbValueIndexes: attribute names, comma-separated - or a JSON array of
// { "attribute": , "type"?: , "tenant"?: }
//
static bool optionParse(const char* option)
{
  static char  buf[4096];
  CorAlloc     alloc;
  bool         ok = true;

  corAllocBufferInit(&alloc, buf, sizeof(buf), 16 * 1024, NULL, "dbIndexes");

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
      COR_E("mongoc: --dbValueIndexes: not a JSON array of { \"attribute\", \"type\", \"tenant\" }");
      ok = false;
    }

    for (CorNode* declP = (treeP != NULL)? treeP->value.head : NULL; (ok == true) && (declP != NULL); declP = declP->next)
    {
      CorNode* attrP   = (declP->type == CorObject)? corTreeLookup(declP, "attribute") : NULL;
      CorNode* typeP   = (declP->type == CorObject)? corTreeLookup(declP, "type")      : NULL;
      CorNode* tenantP = (declP->type == CorObject)? corTreeLookup(declP, "tenant")    : NULL;

      if ((attrP == NULL) || (attrP->type != CorString) || ((typeP != NULL) && (typeP->type != CorString)) || ((tenantP != NULL) && (tenantP->type != CorString)))
      {
        COR_E("mongoc: --dbValueIndexes: each declaration is { \"attribute\": <string>, \"type\"?: <string>, \"tenant\"?: <string> }");
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
// mongocIndexesInit - --dbValueIndexes's declarations, then the admin API's: before any tenant is set up
//
int mongocIndexesInit(void)
{
  pthread_mutex_lock(&declMutex);

  bool ok = ((mongocDbIndexes == NULL) || (optionParse(mongocDbIndexes) == true)) && (declsLoad() == true);

  pthread_mutex_unlock(&declMutex);

  if (declN > 0)
    COR_I("mongoc: %d declared index(es)", declN);

  return ok? 0 : -1;
}



// -----------------------------------------------------------------------------
//
// mongocIndexesApply - the declared indexes of a tenant: the ones of the declarations that cover it
// created, every other cor_v_* / cor_d_* index of its entities collection dropped
//
void mongocIndexesApply(Tenant* tenantP, mongoc_collection_t* collP)
{
  char             errorText[256];
  const bson_t*    docP;
  bson_error_t     error;
  char*            staleV[64];
  int              staleN = 0;

  pthread_mutex_lock(&declMutex);

  mongoc_cursor_t* cursorP = mongoc_collection_find_indexes_with_opts(collP, NULL);

  while (mongoc_cursor_next(cursorP, &docP) && (staleN < (int) (sizeof(staleV) / sizeof(staleV[0]))))
  {
    bson_iter_t  iter;
    const char*  name;

    if ((bson_iter_init_find(&iter, docP, "name") == false) || (BSON_ITER_HOLDS_UTF8(&iter) == false))
      continue;

    name = bson_iter_utf8(&iter, NULL);
    if ((strncmp(name, "cor_v_", 6) != 0) && (strncmp(name, "cor_d_", 6) != 0))
      continue;

    int ix = declLookup(&name[6]);
    if ((ix < 0) || (declCovers(&declV[ix], tenantP) == false))
      staleV[staleN++] = strdup(&name[6]);
  }

  if (mongoc_cursor_error(cursorP, &error))
    COR_W("mongoc: db '%s': listing its indexes: %s", tenantP->dbName, error.message);
  mongoc_cursor_destroy(cursorP);

  for (int ix = 0; ix < staleN; ix++)
  {
    bool dup = false;

    for (int jx = 0; (jx < ix) && (dup == false); jx++)
      dup = (strcmp(staleV[jx], staleV[ix]) == 0);

    if (dup == false)
    {
      COR_I("mongoc: db '%s': the indexes of declaration %s dropped - it is no longer declared", tenantP->dbName, staleV[ix]);
      indexesDrop(collP, staleV[ix], tenantP->dbName);
    }
  }

  for (int ix = 0; ix < staleN; ix++)
    free(staleV[ix]);

  for (int ix = 0; ix < declN; ix++)
  {
    if (declCovers(&declV[ix], tenantP) && (indexesCreate(collP, &declV[ix], errorText, sizeof(errorText)) == false))
      COR_E("mongoc: db '%s': the declared index of '%s' (%s): %s", tenantP->dbName, declV[ix].attr, declV[ix].id, errorText);
  }

  pthread_mutex_unlock(&declMutex);
}



// -----------------------------------------------------------------------------
//
// tenantsEach - every tenant set up, the default one first, its entities collection: f returns false
// to stop
//
typedef bool (*TenantFunc)(Tenant* tenantP, mongoc_collection_t* collP, void* dataP);

static bool tenantsEach(TenantFunc f, void* dataP)
{
  mongoc_client_t*  clientP = mongoc_client_pool_pop(poolP);
  bool              go      = true;

  for (Tenant* tP = &tenant0; (tP != NULL) && (go == true); tP = (tP == &tenant0)? tenantList : tP->next)
  {
    if ((tP != &tenant0) && (tP->initialized == false))
      continue;

    mongoc_collection_t* collP = mongoc_client_get_collection(clientP, tP->dbName, "entities");
    go = f(tP, collP, dataP);
    mongoc_collection_destroy(collP);
  }

  mongoc_client_pool_push(poolP, clientP);
  return go;
}



// -----------------------------------------------------------------------------
//
// IndexWork - what tenantsEach's functions below share
//
typedef struct IndexWork
{
  IndexDecl*  declP;
  char*       errorText;
  int         errorTextLen;
  CorNode*    tenantsP;          // indexStatsAdd: the array to fill
} IndexWork;



static bool createIn(Tenant* tenantP, mongoc_collection_t* collP, void* dataP)
{
  IndexWork* wP = (IndexWork*) dataP;

  if (declCovers(wP->declP, tenantP) == false)
    return true;

  if (indexesCreate(collP, wP->declP, wP->errorText, wP->errorTextLen) == false)
  {
    COR_E("mongoc: db '%s': the declared index of '%s' (%s): %s", tenantP->dbName, wP->declP->attr, wP->declP->id, wP->errorText);
    return false;
  }

  COR_I("mongoc: db '%s': the declared index of '%s' (%s) created", tenantP->dbName, wP->declP->attr, wP->declP->id);
  return true;
}



static bool dropIn(Tenant* tenantP, mongoc_collection_t* collP, void* dataP)
{
  IndexWork* wP = (IndexWork*) dataP;

  indexesDrop(collP, wP->declP->id, tenantP->dbName);
  return true;
}



// -----------------------------------------------------------------------------
//
// indexNumber - the int64 at a dotted path of a document - 0 if not there
//
static int64_t indexNumber(const bson_t* docP, const char* path)
{
  bson_iter_t iter;
  bson_iter_t found;

  if (bson_iter_init(&iter, docP) && bson_iter_find_descendant(&iter, path, &found) && BSON_ITER_HOLDS_NUMBER(&found))
    return bson_iter_as_int64(&found);

  return 0;
}



// -----------------------------------------------------------------------------
//
// statsIn - a declaration's two indexes in a tenant: their bytes ($collStats) and the times a query
// used them ($indexStats, since the database server started)
//
static bool statsIn(Tenant* tenantP, mongoc_collection_t* collP, void* dataP)
{
  IndexWork*     wP = (IndexWork*) dataP;
  char           vName[32];
  char           dName[32];
  int64_t        bytes    = 0;
  int64_t        accesses = 0;
  bool           found    = false;
  const bson_t*  docP;
  bson_error_t   error;

  if (declCovers(wP->declP, tenantP) == false)
    return true;

  snprintf(vName, sizeof(vName), "cor_v_%s", wP->declP->id);
  snprintf(dName, sizeof(dName), "cor_d_%s", wP->declP->id);

  bson_t*          pipelineP = BCON_NEW("pipeline", "[", "{", "$indexStats", "{", "}", "}", "]");
  mongoc_cursor_t* cursorP   = mongoc_collection_aggregate(collP, MONGOC_QUERY_NONE, pipelineP, NULL, NULL);

  while (mongoc_cursor_next(cursorP, &docP))
  {
    bson_iter_t iter;

    if (bson_iter_init_find(&iter, docP, "name") && BSON_ITER_HOLDS_UTF8(&iter))
    {
      const char* name = bson_iter_utf8(&iter, NULL);

      if ((strcmp(name, vName) == 0) || (strcmp(name, dName) == 0))
      {
        accesses += indexNumber(docP, "accesses.ops");
        found     = true;
      }
    }
  }

  if (mongoc_cursor_error(cursorP, &error))
    COR_W("mongoc: db '%s': $indexStats: %s", tenantP->dbName, error.message);
  mongoc_cursor_destroy(cursorP);
  bson_destroy(pipelineP);

  pipelineP = BCON_NEW("pipeline", "[", "{", "$collStats", "{", "storageStats", "{", "}", "}", "}", "]");
  cursorP   = mongoc_collection_aggregate(collP, MONGOC_QUERY_NONE, pipelineP, NULL, NULL);

  if (mongoc_cursor_next(cursorP, &docP))
  {
    char path[64];

    snprintf(path, sizeof(path), "storageStats.indexSizes.%s", vName);
    bytes += indexNumber(docP, path);
    snprintf(path, sizeof(path), "storageStats.indexSizes.%s", dName);
    bytes += indexNumber(docP, path);
  }

  mongoc_cursor_destroy(cursorP);
  bson_destroy(pipelineP);

  CorNode* tP = corTreeObject(corRest.kallocP, NULL);

  corTreeChildAdd(tP, corTreeString(corRest.kallocP, "tenant", tenantP->name));
  if (tenantP->snapshotId != NULL)
    corTreeChildAdd(tP, corTreeString(corRest.kallocP, "snapshot", tenantP->snapshotId));
  corTreeChildAdd(tP, corTreeBoolean(corRest.kallocP, "built", found));
  corTreeChildAdd(tP, corTreeInteger(corRest.kallocP, "bytes", bytes));
  corTreeChildAdd(tP, corTreeInteger(corRest.kallocP, "accesses", accesses));
  corTreeChildAdd(wP->tenantsP, tP);

  return true;
}



// -----------------------------------------------------------------------------
//
// declToTree - a declaration as the admin API shows it (declMutex held); its tenants with stats
//
static CorNode* declToTree(IndexDecl* dP, bool stats)
{
  CorNode* declP = corTreeObject(corRest.kallocP, NULL);

  corTreeChildAdd(declP, corTreeString(corRest.kallocP, "id", dP->id));
  corTreeChildAdd(declP, corTreeString(corRest.kallocP, "attribute", dP->attr));
  if (dP->type != NULL)
    corTreeChildAdd(declP, corTreeString(corRest.kallocP, "type", dP->type));
  if (dP->tenant != NULL)
    corTreeChildAdd(declP, corTreeString(corRest.kallocP, "tenant", dP->tenant));
  corTreeChildAdd(declP, corTreeString(corRest.kallocP, "declaredBy", dP->api? "api" : "dbValueIndexes"));

  if (stats == true)
  {
    IndexWork w = { dP, NULL, 0, corTreeArray(corRest.kallocP, "tenants") };

    tenantsEach(statsIn, &w);
    corTreeChildAdd(declP, w.tenantsP);
  }

  return declP;
}



// -----------------------------------------------------------------------------
//
// mongocIndexList -
//
int mongocIndexList(CorNode** arrayPP)
{
  CorNode* arrayP = corTreeArray(corRest.kallocP, NULL);

  pthread_mutex_lock(&declMutex);

  for (int ix = 0; ix < declN; ix++)
    corTreeChildAdd(arrayP, declToTree(&declV[ix], true));

  pthread_mutex_unlock(&declMutex);

  *arrayPP = arrayP;
  return DB_OK;
}



// -----------------------------------------------------------------------------
//
// mongocIndexAdd - a declaration of the admin API: its indexes created in every tenant it covers, then
// kept. An index that cannot be created: none of them kept, the declaration not made (DB_ERR)
//
int mongocIndexAdd(const char* attr, const char* type, const char* tenant, CorNode** declPP, char* errorText, int errorTextLen)
{
  pthread_mutex_lock(&declMutex);

  int rc = declAdd(attr, type, tenant, true, errorText, errorTextLen);

  if (rc == DB_OK)
  {
    IndexWork w = { &declV[declN - 1], errorText, errorTextLen, NULL };

    if ((tenantsEach(createIn, &w) == false) || (declsSave(errorText, errorTextLen) == false))
    {
      tenantsEach(dropIn, &w);
      declFree(&declV[--declN]);
      rc = DB_ERR;
    }
    else
      *declPP = declToTree(&declV[declN - 1], false);
  }

  pthread_mutex_unlock(&declMutex);
  return rc;
}



// -----------------------------------------------------------------------------
//
// mongocIndexDelete - a declaration of the admin API taken away: its indexes dropped in every tenant.
// One of --dbValueIndexes is not (DB_BAD_INPUT): it is the start's, and comes back with it
//
int mongocIndexDelete(const char* id, char* errorText, int errorTextLen)
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
    snprintf(errorText, errorTextLen, "'%s' is declared by --dbValueIndexes - taken out of it, it is gone at the next start", id);
    rc = DB_BAD_INPUT;
  }
  else
  {
    IndexDecl  d = declV[ix];
    IndexWork  w = { &d, errorText, errorTextLen, NULL };

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
      tenantsEach(dropIn, &w);
      declFree(&d);
    }
  }

  pthread_mutex_unlock(&declMutex);
  return rc;
}
