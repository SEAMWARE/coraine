//
// FILE            dbInit.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <dlfcn.h>                                    // dlclose
#include <stddef.h>                                   // NULL

#include "ktrace/kTrace.h"                            // KT_E

#include "db/DbDriver.h"                              // DbDriver
#include "db/dbInit.h"                                // Own interface



// -----------------------------------------------------------------------------
//
// db - global driver instance (filled by pluginLoadDb via dbRegister)
//
DbDriver db;



// -----------------------------------------------------------------------------
//
// dbStart - call db.init() to connect to the database
//
int dbStart(void)
{
  if (db.init == NULL)
  {
    KT_E("db plugin has no init function");
    return DB_ERR;
  }

  int r = db.init();
  if (r != DB_OK)
  {
    KT_E("db driver init failed (rc=%d)", r);
    return r;
  }

  return DB_OK;
}
