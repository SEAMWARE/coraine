//
// FILE            mongocClose.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stdlib.h>                                  // free

#include <mongoc/mongoc.h>                           // mongoc_client_pool_destroy, mongoc_cleanup

#include "ktrace/kTrace.h"                               // KT_I

#include "currentState/mongoc/mongocClose.h"                      // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocClose -
//
void mongocClose(void)
{
  if (poolP != NULL)
  {
    mongoc_client_pool_destroy(poolP);
    poolP = NULL;
  }

  mongoc_cleanup();

  KT_I("mongoc: closed");
}
