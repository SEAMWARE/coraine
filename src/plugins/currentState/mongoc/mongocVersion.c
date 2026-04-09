//
// FILE            mongocVersion.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <string.h>                                    // strcpy

#include <mongoc/mongoc.h>                             // MONGOC_VERSION_S, mongoc_client_*

#include "ktrace/kTrace.h"                                 // KT_E
#include "kjson/kjBuilder.h"                           // kjObject, kjString, kjChildAdd
#include "kjson/kjBufferCreate.h"                      // kjBufferCreate

#include "currentState/mongoc/mongocVersion.h"         // Own interface



// -----------------------------------------------------------------------------
//
// Shared state - pool from mongocInit.c
//
extern mongoc_client_pool_t* poolP;



// -----------------------------------------------------------------------------
//
// mongocServerVersion - filled by mongocServerVersionGet()
//
static char mongocServerVersion[128] = "unknown";



// -----------------------------------------------------------------------------
//
// MONGOC_PLUGIN_VERSION - version of the mongoc plugin itself
//
#ifndef MONGOC_PLUGIN_VERSION
#define MONGOC_PLUGIN_VERSION "post-0.2.0"
#endif



// -----------------------------------------------------------------------------
//
// mongocServerVersionGet - query the MongoDB server's version via buildinfo
//
int mongocServerVersionGet(void)
{
  if (poolP == NULL)
    return -1;

  mongoc_client_t* clientP = mongoc_client_pool_pop(poolP);
  bson_t           command;
  bson_t           reply;
  bson_error_t     error;

  bson_init(&command);
  BSON_APPEND_INT32(&command, "buildinfo", 1);

  bool ok = mongoc_client_command_simple(clientP, "admin", &command, NULL, &reply, &error);
  bson_destroy(&command);

  if (!ok)
  {
    bson_destroy(&reply);
    mongoc_client_pool_push(poolP, clientP);
    KT_E("mongoc: buildinfo command failed: %s", error.message);
    return -1;
  }

  //
  // Extract "version" from the reply
  //
  bson_iter_t iter;
  if (bson_iter_init_find(&iter, &reply, "version") && BSON_ITER_HOLDS_UTF8(&iter))
  {
    const char* v = bson_iter_utf8(&iter, NULL);
    strncpy(mongocServerVersion, v, sizeof(mongocServerVersion) - 1);
    mongocServerVersion[sizeof(mongocServerVersion) - 1] = '\0';
  }

  bson_destroy(&reply);
  mongoc_client_pool_push(poolP, clientP);

  return 0;
}



// -----------------------------------------------------------------------------
//
// mongocVersionInfo - add version entries to the root object
//
void mongocVersionInfo(KAlloc* allocP, KjNode* root)
{
  Kjson   kjsonLocal;
  Kjson*  kjsonP = kjBufferCreate(&kjsonLocal, allocP);

  kjChildAdd(root, kjString(kjsonP, "mongoc plugin",  MONGOC_PLUGIN_VERSION));
  kjChildAdd(root, kjString(kjsonP, "mongoc driver",  MONGOC_VERSION_S));
  kjChildAdd(root, kjString(kjsonP, "mongodb server", mongocServerVersion));
}
