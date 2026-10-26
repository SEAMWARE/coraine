//
// FILE            storePause.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                   // bool
#include <stdint.h>                                    // uint64_t
#include <string.h>                                    // strcmp, strncmp
#include <time.h>                                      // clock_gettime
#include <stdatomic.h>                                 // atomic_*

#include "corRest/CorRestState.h"                      // corRest
#include "corRest/corRestOutHeader.h"                  // corRestOutHeaderAdd
#include "corNgsild/ldError.h"                         // ldError

#include "db/DbDriver.h"                               // db

#include "storePause.h"                                // Own interface



// -----------------------------------------------------------------------------
//
// refused - requests refused for the pause, since the start
//
static _Atomic uint64_t refused = 0;



// -----------------------------------------------------------------------------
//
// writesNothing - a request that is served while the store is paused
//
static bool writesNothing(const char* path)
{
  CorRestVerb verb = corRest.in.verb;

  if ((verb == CorVerbGet) || (verb == CorVerbHead) || (verb == CorVerbOptions))
    return true;

  if ((verb == CorVerbPost) &&
      ((strcmp(path, "/ngsi-ld/v1/entityOperations/query") == 0) ||
       (strcmp(path, "/ngsi-ld/v1/temporal/entityOperations/query") == 0) ||
       (strcmp(path, "/ngsi-ld/v1/temporal/timepoint/entityOperations/query") == 0)))
    return true;

  return strcmp(path, "/admin/log") == 0;
}



// -----------------------------------------------------------------------------
//
// storePauseAdmit -
//
bool storePauseAdmit(void)
{
  if ((db.paused == NULL) || (db.paused(NULL, 0, NULL) == false))
    return true;

  const char* path = (corRest.in.urlPath != NULL) ? corRest.in.urlPath : "";

  if (writesNothing(path) == true)
    return true;

  char     reason[200];
  uint64_t sinceNs = 0;

  reason[0] = 0;
  if (db.paused(reason, sizeof(reason), &sinceNs) == false)   // ended meanwhile
    return true;

  struct timespec now;

  clock_gettime(CLOCK_REALTIME, &now);

  uint64_t nowNs = (uint64_t) now.tv_sec * 1000000000ULL + (uint64_t) now.tv_nsec;
  long     secs  = (nowNs > sinceNs) ? (long) ((nowNs - sinceNs) / 1000000000ULL) : 0;

  atomic_fetch_add_explicit(&refused, 1, memory_order_relaxed);
  ldError(503, "https://coraine.readthedocs.io/errors/PersistenceUnavailable", "Persistence Unavailable",
          "writes are refused until the broker can write its log again (paused for %ld s): %s", secs, reason);
  corRestOutHeaderAdd("Retry-After", "5");

  return false;
}



// -----------------------------------------------------------------------------
//
// storePauseValues -
//
bool storePauseValues(uint64_t* sinceNsP, char* reason, int reasonLen, uint64_t* refusedP)
{
  *refusedP = atomic_load_explicit(&refused, memory_order_relaxed);
  *sinceNsP = 0;
  reason[0] = 0;

  return (db.paused != NULL) && db.paused(reason, reasonLen, sinceNsP);
}
