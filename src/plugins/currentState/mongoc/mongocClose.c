//
// FILE            mongocClose.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdlib.h>                                  // free

#include <mongoc/mongoc.h>                           // mongoc_client_pool_destroy, mongoc_cleanup

#include "corLog/corLog.h"                               // COR_I

#include "shared/geoMatch.h"                             // geoMatchClose
#if COR_FEATURE_HEALTH
#include "currentState/mongoc/mongocPing.h"                       // mongocPingClose
#endif
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

#if COR_FEATURE_HEALTH
  mongocPingClose();   // the broker has stopped the health port's pinger (healthStop) before dbClose
#endif

  geoMatchClose();   // shared GEOS ctx (geoMatchInit at startup) — sub-notify / CSR geo-match

  mongoc_cleanup();

  COR_I("mongoc: closed");
}
