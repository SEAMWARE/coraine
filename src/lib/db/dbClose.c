//
// FILE            dbClose.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                   // NULL

#include "swPlugin/swPlugin.h"                        // swPluginCloseAll
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

  swPluginCloseAll();
}
