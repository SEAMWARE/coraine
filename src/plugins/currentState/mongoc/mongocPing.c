//
// FILE            mongocPing.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                    // NULL

#include <mongoc/mongoc.h>                             // mongoc_client_*, mongoc_uri_*

#include "db/DbDriver.h"                               // DB_OK, DB_ERR
#include "currentState/mongoc/mongocGlobals.h"         // mongocUriString
#include "currentState/mongoc/mongocPing.h"            // Own interface



// -----------------------------------------------------------------------------
//
// clientP - the ping's own client, single-threaded: only the health port's pinger uses it
//
// Not one from the pool. A loaded broker can have every pooled client out, and mongoc_client_pool_pop
// then waits for a request to hand one back - the ping would measure the broker's load, and hang with
// it, instead of answering whether mongod is there. Its own client, with every timeout at timeoutMs, is
// bounded whatever the requests are doing.
//
static mongoc_client_t* clientP = NULL;



// -----------------------------------------------------------------------------
//
// clientCreate - a client on the broker's URI, its timeouts all at timeoutMs
//
static mongoc_client_t* clientCreate(int timeoutMs)
{
  bson_error_t  error;
  mongoc_uri_t* uriP = mongoc_uri_new_with_error(mongocUriString, &error);

  if (uriP == NULL)
    return NULL;

  mongoc_uri_set_option_as_int32(uriP, MONGOC_URI_SERVERSELECTIONTIMEOUTMS, timeoutMs);
  mongoc_uri_set_option_as_int32(uriP, MONGOC_URI_CONNECTTIMEOUTMS,         timeoutMs);
  mongoc_uri_set_option_as_int32(uriP, MONGOC_URI_SOCKETTIMEOUTMS,          timeoutMs);

  mongoc_client_t* cP = mongoc_client_new_from_uri(uriP);
  mongoc_uri_destroy(uriP);

  return cP;
}



// -----------------------------------------------------------------------------
//
// mongocPing -
//
// After a failure the client is destroyed and the next ping makes a new one: a single-threaded mongoc
// client that failed to reach a server waits 5 s before it tries that server again, and the health
// port would report mongod gone for 5 s after it is back.
//
int mongocPing(int timeoutMs)
{
  if ((clientP == NULL) && ((clientP = clientCreate(timeoutMs)) == NULL))
    return DB_ERR;

  bson_t       ping;
  bson_t       reply;
  bson_error_t error;

  bson_init(&ping);
  BSON_APPEND_INT32(&ping, "ping", 1);

  bool ok = mongoc_client_command_simple(clientP, "admin", &ping, NULL, &reply, &error);

  bson_destroy(&ping);
  bson_destroy(&reply);

  if (ok == false)
  {
    mongoc_client_destroy(clientP);
    clientP = NULL;
    return DB_ERR;
  }

  return DB_OK;
}



// -----------------------------------------------------------------------------
//
// mongocPingClose -
//
void mongocPingClose(void)
{
  if (clientP != NULL)
  {
    mongoc_client_destroy(clientP);
    clientP = NULL;
  }
}
