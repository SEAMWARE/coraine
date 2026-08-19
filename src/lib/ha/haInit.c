//
// FILE            haInit.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stddef.h>                                      // NULL
#include <string.h>                                      // strcmp
#include <unistd.h>                                      // usleep

#include "ktrace/kTrace.h"                               // KT_*

#include "db/DbDriver.h"                                 // db
#include "coraineTraceLevels.h"                         // KtHa
#include "ha/haEventApply.h"                             // haEventApply
#include "ha/haInit.h"                                   // Own interface



// -----------------------------------------------------------------------------
//
// haChannel - the --ha option (see haInit.h)
//
char* haChannel = NULL;



// -----------------------------------------------------------------------------
//
// haApplyEnabled - are the caches loaded?
//
// The channel is started BEFORE the caches are loaded, on purpose (see haInit.h),
// so an event can arrive before there is anything to apply it to. It waits here.
//
// A condition variable would do the same thing with more machinery: this is
// waited on once per broker start, by one thread, for a few milliseconds at
// most. 'volatile' is what makes the poll actually re-read it.
//
static volatile bool haApplyEnabled = false;



// -----------------------------------------------------------------------------
//
// haApplyEnable -
//
void haApplyEnable(void)
{
  haApplyEnabled = true;
}



// -----------------------------------------------------------------------------
//
// haApplyWait -
//
void haApplyWait(void)
{
  while (haApplyEnabled == false)
    usleep(10000);
}



// -----------------------------------------------------------------------------
//
// haInit -
//
bool haInit(void)
{
  if ((haChannel == NULL) || (haChannel[0] == 0))
  {
    KT_T(KtHa, "HA is off (no --ha)");
    return true;
  }

  if (strcmp(haChannel, "mongo") == 0)
  {
    //
    // The change feed is the database's own, so only the DB plugin can produce
    // it - and only a plugin whose store is shared by the other instances in the
    // first place. An in-memory store is nobody else's: there is no change to
    // learn about, and pretending otherwise would leave the deployment believing
    // its caches are in sync.
    //
    if (db.haWatchStart == NULL)
      KT_X(1, "--ha mongo needs a database plugin that can report what another broker instance wrote; the '%s' plugin cannot. "
              "Point the broker at a shared database, or use '--ha <ip:port>' once the haaux server exists",
           (db.alias != NULL)? db.alias : "current");

    KT_T(KtHa, "HA: the database change feed is the channel");

    return (db.haWatchStart(haEventApply) == DB_OK);
  }

  //
  // Anything else is an address: the haaux server, which the broker connects to
  // over a socket. That is the channel for a deployment whose database has no
  // change feed - or no shared database at all - and it is not written yet, so
  // say so rather than start up pretending HA is on.
  //
  KT_X(1, "--ha %s: an address means the haaux server, which is not implemented yet. Use '--ha mongo'", haChannel);

  return false;
}
