//
// FILE            mongocStorageFormat.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The storage format of a database (doc/installation.md, "Storage format") - one integer, in the database itself:
//
//   <db>.metadata   { _id: "storageFormat", version: <n> }
//
// in the default tenant's database and in every tenant's. A broker refuses a database whose format is
// newer than the newest it knows: what a newer release wrote, an older one misreads - and writes back
// misread.
//
#include <stdbool.h>                                   // bool
#include <stdint.h>                                    // int64_t
#include <string.h>                                    // strcmp

#include <mongoc/mongoc.h>                             // mongoc_*, bson_*

#include "corLog/corLog.h"                             // COR_E, COR_I

#include "currentState/mongoc/mongocStorageFormat.h"   // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// StorageFormat - one format this plugin reads and writes, and the step that brings a database in the
// format before it up to it
//
// A change to what a document looks like in the database, that a release before it would misread,
// is a new line here, newest last - and its upgrade step, if a database in the older format is not
// read as it is.
//
typedef struct StorageFormat
{
  int          version;
  const char*  what;
  bool       (*upgrade)(const char* dbName);           // NULL: the format before it is read as it is
} StorageFormat;

#if COR_FEATURE_Q_PREFILTER
// -----------------------------------------------------------------------------
//
// datasetIdsMark - storage format 2: every attribute with an instance of a datasetId marked
// ("@datasetIds": true next to its instances, mongocTreeToBson.c) - one update of the whole collection,
// done in the database: an entity member that is an object is an attribute (its dataset wrapper), and
// one with a member that is an object and not "@none" holds an instance of a datasetId
//
static bool datasetIdsMark(const char* dbName)
{
  const char* pipeline =
    "{ \"0\": { \"$replaceWith\": { \"$arrayToObject\": { \"$map\": {"
    "    \"input\": { \"$objectToArray\": \"$$ROOT\" }, \"as\": \"m\","
    "    \"in\": { \"$cond\": ["
    "      { \"$and\": ["
    "        { \"$eq\": [ { \"$type\": \"$$m.v\" }, \"object\" ] },"
    "        { \"$anyElementTrue\": [ { \"$map\": {"
    "            \"input\": { \"$objectToArray\": \"$$m.v\" }, \"as\": \"i\","
    "            \"in\": { \"$and\": [ { \"$ne\": [ \"$$i.k\", \"@none\" ] },"
    "                                  { \"$ne\": [ \"$$i.k\", \"@datasetIds\" ] },"
    "                                  { \"$eq\": [ { \"$type\": \"$$i.v\" }, \"object\" ] } ] } } } ] }"
    "      ] },"
    "      { \"k\": \"$$m.k\", \"v\": { \"$mergeObjects\": [ \"$$m.v\", { \"@datasetIds\": true } ] } },"
    "      \"$$m\""
    "    ] }"
    "} } } } }";

  bson_error_t          error;
  bson_t*               updateP = bson_new_from_json((const uint8_t*) pipeline, -1, &error);

  if (updateP == NULL)
  {
    COR_E("mongoc: database '%s': the datasetId marks: %s", dbName, error.message);
    return false;
  }

  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, dbName, "entities");
  bson_t                filter  = BSON_INITIALIZER;
  bson_t                reply;
  bool                  ok      = mongoc_collection_update_many(collP, &filter, updateP, NULL, &reply, &error);

  if (ok == false)
    COR_E("mongoc: database '%s': the datasetId marks: %s", dbName, error.message);
  else
  {
    bson_iter_t iter;
    int64_t     marked = 0;

    if (bson_iter_init_find(&iter, &reply, "modifiedCount") && BSON_ITER_HOLDS_NUMBER(&iter))
      marked = bson_iter_as_int64(&iter);

    COR_I("mongoc: database '%s': %ld entities with an instance of a datasetId marked", dbName, (long) marked);
  }

  bson_destroy(&reply);
  bson_destroy(&filter);
  bson_destroy(updateP);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return ok;
}
#endif



static const StorageFormat formatV[] =
{
  //
  // coraine 0.5.0: system timestamps once per created entity - a time that is not in the document is
  // the entity's createdAt - and attribute types once per entity. A database of 0.4.x (no version
  // recorded) is read as it is; 0.4.x reads what 0.5.0 writes WRONG (attributes without type and
  // createdAt).
  //
  { 1, "system timestamps and attribute types once per entity (coraine 0.5.0)", NULL },

#if COR_FEATURE_Q_PREFILTER
  //
  // coraine 0.6.0: an attribute with an instance of a datasetId is marked so - the q pre-filter takes an
  // attribute without the mark for one with its default instance only. A release before it reads the
  // mark as an instance, and writes an instance of a datasetId without it: what q then misses.
  //
  { 2, "attributes with an instance of a datasetId marked (coraine 0.6.0)", datasetIdsMark }
#endif
};



// -----------------------------------------------------------------------------
//
// formatNewest - the newest format this build knows - the one it writes
//
static int formatNewest(void)
{
  return formatV[sizeof(formatV) / sizeof(formatV[0]) - 1].version;
}



// -----------------------------------------------------------------------------
//
// versionRead - the version recorded in the database: 0 if none is, -1 on an error (said why)
//
static int versionRead(mongoc_collection_t* collP, const char* dbName)
{
  bson_t           filter;
  const bson_t*    docP;
  bson_error_t     error;
  int              version = 0;

  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", "storageFormat");

  mongoc_cursor_t* cursorP = mongoc_collection_find_with_opts(collP, &filter, NULL, NULL);

  if (mongoc_cursor_next(cursorP, &docP))
  {
    bson_iter_t iter;

    if (bson_iter_init_find(&iter, docP, "version") && BSON_ITER_HOLDS_NUMBER(&iter))
      version = (int) bson_iter_as_int64(&iter);
    else
    {
      COR_E("mongoc: database '%s': metadata.storageFormat has no numeric 'version'", dbName);
      version = -1;
    }
  }

  if (mongoc_cursor_error(cursorP, &error))
  {
    COR_E("mongoc: database '%s': reading its storage format: %s", dbName, error.message);
    version = -1;
  }

  mongoc_cursor_destroy(cursorP);
  bson_destroy(&filter);

  return version;
}



// -----------------------------------------------------------------------------
//
// hasData - does the database hold anything but its metadata?
//
static bool hasData(mongoc_client_t* clientP, const char* dbName)
{
  mongoc_database_t* dbP   = mongoc_client_get_database(clientP, dbName);
  bson_error_t       error;
  char**             nameV = mongoc_database_get_collection_names_with_opts(dbP, NULL, &error);
  bool               data  = false;

  for (int ix = 0; (nameV != NULL) && (nameV[ix] != NULL); ix++)
  {
    if (strcmp(nameV[ix], "metadata") != 0)
      data = true;
  }

  bson_strfreev(nameV);
  mongoc_database_destroy(dbP);

  return data;
}



// -----------------------------------------------------------------------------
//
// versionWrite - this build's version recorded; false on an error (said why)
//
static bool versionWrite(mongoc_collection_t* collP, const char* dbName, int version)
{
  bson_t        selector;
  bson_t        update;
  bson_t        set;
  bson_t        opts;
  bson_error_t  error;

  bson_init(&selector);
  BSON_APPEND_UTF8(&selector, "_id", "storageFormat");

  bson_init(&update);
  BSON_APPEND_DOCUMENT_BEGIN(&update, "$set", &set);
  BSON_APPEND_INT32(&set, "version", version);
  bson_append_document_end(&update, &set);

  bson_init(&opts);
  BSON_APPEND_BOOL(&opts, "upsert", true);

  bool ok = mongoc_collection_update_one(collP, &selector, &update, &opts, NULL, &error);

  if (ok == false)
    COR_E("mongoc: database '%s': recording its storage format %d: %s", dbName, version, error.message);

  bson_destroy(&opts);
  bson_destroy(&update);
  bson_destroy(&selector);

  return ok;
}



// -----------------------------------------------------------------------------
//
// mongocStorageFormat -
//
int mongocStorageFormat(const char* dbName, bool write)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, dbName, "metadata");
  int                   newest  = formatNewest();
  int                   found   = versionRead(collP, dbName);
  int                   rc      = 0;

  if (found < 0)
    rc = -1;
  else if (found > newest)
  {
    COR_E("mongoc: database '%s' is in storage format %d - this build of coraine knows formats up to %d. "
          "It was written by a newer release of coraine, and this one would misread it: run the release that wrote it, or a newer one. "
          "A downgrade is not supported once a newer release has written to a database - doc/installation.md, Storage format",
          dbName, found, newest);
    rc = -1;
  }
  else if ((found < newest) && (write == true))
  {
    if ((found == 0) && (hasData(clientP, dbName) == false))
      COR_I("mongoc: database '%s' is new - storage format %d", dbName, newest);
    else
    {
      //
      // The upgrade steps of every format after the one found, in order
      //
      for (unsigned int ix = 0; ix < sizeof(formatV) / sizeof(formatV[0]); ix++)
      {
        if ((formatV[ix].version > found) && (formatV[ix].upgrade != NULL) && (formatV[ix].upgrade(dbName) == false))
        {
          COR_E("mongoc: database '%s': the upgrade to storage format %d (%s) failed", dbName, formatV[ix].version, formatV[ix].what);
          rc = -1;
          break;
        }
      }

      if (rc == 0)
      {
        if (found == 0)
          COR_I("mongoc: database '%s' has no storage format recorded (coraine 0.4.x or earlier) - read as it is, now storage format %d", dbName, newest);
        else
          COR_I("mongoc: database '%s' upgraded from storage format %d to %d", dbName, found, newest);
      }
    }

    if ((rc == 0) && (versionWrite(collP, dbName, newest) == false))
      rc = -1;
  }

  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return rc;
}
