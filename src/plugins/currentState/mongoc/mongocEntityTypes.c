//
// FILE            mongocEntityTypes.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <mongoc/mongoc.h>                           // mongoc_collection_t, mongoc_collection_find_with_opts

#include "corLog/corLog.h"                               // COR_E
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corRest/CorRestState.h"                      // corRest

#include "db/DbDriver.h"                             // DB_OK, DB_NOT_FOUND, DB_ERR
#include "currentState/mongoc/mongocBsonToTree.h"                 // mongocBsonToTree
#include "currentState/mongoc/mongocEntityTypes.h"                // Own interface



// -----------------------------------------------------------------------------
//
// The client pool - mongocInit
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocEntityTypes - the document projected to its "type": mongod sends nothing else
//
int mongocEntityTypes(Tenant* tenantP, const char* entityId, CorNode** typePP)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");
  bson_t                filter;
  bson_t                opts;
  bson_t                projection;

  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", entityId);

  bson_init(&opts);
  BSON_APPEND_DOCUMENT_BEGIN(&opts, "projection", &projection);
  BSON_APPEND_INT32(&projection, "type", 1);
  bson_append_document_end(&opts, &projection);

  mongoc_cursor_t* cursorP = mongoc_collection_find_with_opts(collP, &filter, &opts, NULL);
  const bson_t*    doc;
  int              result;

  *typePP = NULL;

  if (mongoc_cursor_next(cursorP, &doc))
  {
    CorNode* treeP = mongocBsonToTree(&corRest.kalloc, doc);

    *typePP = (treeP != NULL) ? corTreeLookup(treeP, "type") : NULL;
    result  = DB_OK;
  }
  else
  {
    bson_error_t error;

    if (mongoc_cursor_error(cursorP, &error))
    {
      COR_E("mongoc: entityTypes failed: %s", error.message);
      result = DB_ERR;
    }
    else
      result = DB_NOT_FOUND;
  }

  bson_destroy(&opts);
  bson_destroy(&filter);
  mongoc_cursor_destroy(cursorP);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}
