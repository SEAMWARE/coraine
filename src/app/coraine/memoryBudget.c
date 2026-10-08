//
// FILE            memoryBudget.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                   // bool
#include <stdint.h>                                    // uint64_t
#include <stdio.h>                                     // snprintf, sscanf
#include <string.h>                                    // strcmp, strncmp
#include <unistd.h>                                    // sysconf
#include <time.h>                                      // nanosleep, clock_gettime
#include <malloc.h>                                    // malloc_trim
#include <pthread.h>                                   // pthread_create, pthread_detach
#include <stdatomic.h>                                 // _Atomic, atomic_*

#include "corBase/corFileReadInto.h"                   // corFileReadInto
#include "corBase/corMemoryLimit.h"                    // corMemoryLimit
#include "corLog/corLog.h"                             // COR_V, COR_W
#include "corRest/CorRestState.h"                      // corRest
#include "corRest/corRestOutHeader.h"                  // corRestOutHeaderAdd
#include "corNgsild/ldError.h"                         // ldError

#include "memoryBudget.h"                              // Own interface



// -----------------------------------------------------------------------------
//
// State - set once by memoryBudgetInit, then read by every request
//
static uint64_t          budget    = 0;                // 0: no budget, nothing checked
static uint64_t          soft      = 0;
static _Atomic uint64_t  used      = 0;                // RssAnon + RssShmem - what the budget is compared with
static _Atomic uint64_t  refused   = 0;
static long              pageSize  = 4096;



// -----------------------------------------------------------------------------
//
// residentRead - the process's resident set, in bytes (/proc/self/statm, second field, in pages)
//
static uint64_t residentRead(void)
{
  char               buf[128];
  unsigned long long size;
  unsigned long long pages;

  if ((corFileReadInto("/proc/self/statm", buf, sizeof(buf)) <= 0) || (sscanf(buf, "%llu %llu", &size, &pages) != 2))
    return 0;

  return (uint64_t) pages * (uint64_t) pageSize;
}



// -----------------------------------------------------------------------------
//
// usedRead - what counts against the budget: anonymous and shared memory (RssAnon + RssShmem, in bytes)
//
// Not the whole resident set: its file-backed pages (RssFile - the executable, the libraries, corDB's
// memory-mapped log, up to 1 GiB a segment) are page cache the kernel writes back and reclaims before
// it kills anything. Counted, a persistent corDB under writes would be refused for its log while the
// memory it actually holds is a fraction of the budget.
//
static uint64_t usedRead(void)
{
  char               buf[4096];
  unsigned long long kib  = 0;
  int                seen = 0;

  if (corFileReadInto("/proc/self/status", buf, sizeof(buf)) <= 0)
    return 0;

  for (char* line = buf; (line != NULL) && (*line != 0) && (seen < 2); line = strchr(line, '\n'))
  {
    unsigned long long v;

    if (*line == '\n')
      line++;

    if ((sscanf(line, "RssAnon: %llu", &v) == 1) || (sscanf(line, "RssShmem: %llu", &v) == 1))
    {
      kib += v;
      seen++;
    }
  }

  return (uint64_t) kib * 1024;
}



// -----------------------------------------------------------------------------
//
// monotonicMs -
//
static int64_t monotonicMs(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}



// -----------------------------------------------------------------------------
//
// sampler - keep 'resident' current, every 100 ms - and trim the heap when over the soft limit
//
static void* sampler(void* arg)
{
  struct timespec period = { 0, 100 * 1000 * 1000 };

  (void) arg;

  int64_t lastTrimMs = 0;

  while (true)
  {
    uint64_t inUse = usedRead();

    //
    // Over the soft limit: give back what is freed but still held. glibc keeps freed memory on its free
    // lists - reused, but resident - and the requests that pushed the broker over the limit have long
    // freed their arenas by the time it is measured; without this the resident set would stay at its
    // high-water mark and the broker refuse requests with little actually in use. At most every 2 s,
    // and never below the soft limit, where it would only cost time.
    //
    if ((inUse >= soft) && (monotonicMs() - lastTrimMs >= 2000))
    {
      malloc_trim(0);
      lastTrimMs = monotonicMs();
      inUse      = usedRead();
    }

    atomic_store_explicit(&used, inUse, memory_order_relaxed);
    nanosleep(&period, NULL);
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// memoryBudgetInit -
//
void memoryBudgetInit(int limitMiB)
{
  pageSize = sysconf(_SC_PAGESIZE);

  uint64_t cgroup = corMemoryLimit();

  if (limitMiB > 0)
    budget = (uint64_t) limitMiB * 1024 * 1024;
  else if (cgroup > 0)
    budget = cgroup / 100 * 85;

  if (budget == 0)
  {
    COR_V("memory budget: none (no --memoryLimit, no cgroup memory limit)");
    return;
  }

  if ((cgroup > 0) && (budget > cgroup))
    COR_W("memory budget of %llu MiB is above the container's limit of %llu MiB - the kernel kills first",
          (unsigned long long) (budget >> 20), (unsigned long long) (cgroup >> 20));

  soft = budget / 100 * 90;
  atomic_store_explicit(&used, usedRead(), memory_order_relaxed);

  pthread_t tid;
  if (pthread_create(&tid, NULL, sampler, NULL) == 0)
    pthread_detach(tid);

  COR_V("memory budget: %llu MiB (writes refused above %llu MiB)", (unsigned long long) (budget >> 20), (unsigned long long) (soft >> 20));
}



// -----------------------------------------------------------------------------
//
// freesOrReports - a request that is let through even over the hard limit
//
// What frees memory (a delete) or tells the operator what is going on - refusing those would leave
// a broker that cannot be helped.
//
static bool freesOrReports(const char* path)
{
  if (corRest.in.verb == CorVerbDelete)
    return true;

  if (strcmp(path, "/ngsi-ld/v1/entityOperations/delete") == 0)
    return true;

  return (strcmp(path, "/version") == 0) || (strcmp(path, "/metrics") == 0) || (strncmp(path, "/admin/", 7) == 0);
}



// -----------------------------------------------------------------------------
//
// growsMemory - a write that may leave more behind than it found (refused already over the soft limit)
//
static bool growsMemory(const char* path)
{
  if ((corRest.in.verb != CorVerbPost) && (corRest.in.verb != CorVerbPut) && (corRest.in.verb != CorVerbPatch))
    return false;

  return (strcmp(path, "/ngsi-ld/v1/entityOperations/delete") != 0) && (strcmp(path, "/ngsi-ld/v1/entityOperations/query") != 0);
}



// -----------------------------------------------------------------------------
//
// memoryBudgetAdmit -
//
bool memoryBudgetAdmit(void)
{
  if (budget == 0)
    return true;

  uint64_t    rss  = atomic_load_explicit(&used, memory_order_relaxed);
  const char* path = (corRest.in.urlPath != NULL) ? corRest.in.urlPath : "";

  if (rss < soft)
    return true;

  if ((rss < budget) ? (growsMemory(path) == false) : (freesOrReports(path) == true))
    return true;

  atomic_fetch_add_explicit(&refused, 1, memory_order_relaxed);
  ldError(503, "https://coraine.readthedocs.io/errors/MemoryBudgetExceeded", "Memory Budget Exceeded",
          "the broker uses %llu MiB of its %llu MiB memory budget - %s are refused until it is back below 90%% of it",
          (unsigned long long) (rss >> 20), (unsigned long long) (budget >> 20),
          (rss < budget) ? "writes" : "requests");
  corRestOutHeaderAdd("Retry-After", "5");

  return false;
}



// -----------------------------------------------------------------------------
//
// memoryBudgetValues -
//
void memoryBudgetValues(uint64_t* budgetP, uint64_t* usedP, uint64_t* residentP, uint64_t* refusedP)
{
  *budgetP   = budget;
  *usedP     = (budget != 0) ? atomic_load_explicit(&used, memory_order_relaxed) : usedRead();
  *residentP = residentRead();
  *refusedP  = atomic_load_explicit(&refused, memory_order_relaxed);
}
