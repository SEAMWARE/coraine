//
// FILE            dbClose.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stddef.h>                                   // NULL

#include "corPlugin/corPlugin.h"                        // corPluginCloseAll
#include "db/DbDriver.h"                              // DbDriver, db
#include "db/dbClose.h"                               // Own interface



// -----------------------------------------------------------------------------
//
// dbClose -
//
void dbClose(void)
{
  if (db.close != NULL)
    db.close();

  corPluginCloseAll();
}
