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
#include <stdlib.h>                                    // strtoull
#include <string.h>                                    // strcmp, strncmp, strchr, strrchr, strlen
#include <fcntl.h>                                     // open, O_RDONLY
#include <unistd.h>                                    // read, close, sysconf
#include <time.h>                                      // nanosleep
#include <pthread.h>                                   // pthread_create, pthread_detach
#include <stdatomic.h>                                 // _Atomic, atomic_*

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
static _Atomic uint64_t  resident  = 0;
static _Atomic uint64_t  refused   = 0;
static long              pageSize  = 4096;



// -----------------------------------------------------------------------------
//
// fileRead - the first bytes of a small file (procfs, cgroupfs), NUL-terminated; false if unreadable
//
static bool fileRead(const char* path, char* buf, int bufSize)
{
  int fd = open(path, O_RDONLY);

  if (fd < 0)
    return false;

  int n = read(fd, buf, bufSize - 1);
  close(fd);

  if (n <= 0)
    return false;

  buf[n] = 0;
  return true;
}



// -----------------------------------------------------------------------------
//
// cgroupLimit - the memory limit this process lives under, in bytes; 0 if there is none
//
// cgroup v2: the process's own cgroup is named in /proc/self/cgroup ("0::/kubepods/.../<container>"),
// and the limit that kills it may sit on it OR on any cgroup above it - a Kubernetes pod limit is the
// parent's. So the walk goes from the process's cgroup up to the root, and the smallest memory.max on
// the way is the one ("max" = none at that level). Inside a container with its own cgroup namespace
// the path is "/", and the walk is the single file /sys/fs/cgroup/memory.max.
//
// cgroup v1 (memory.limit_in_bytes, "unlimited" = a huge page-rounded number) only at the mount root.
//
static uint64_t cgroupLimit(void)
{
  char     buf[512];
  char     path[1024];
  uint64_t limit = 0;

  if ((fileRead("/proc/self/cgroup", buf, sizeof(buf)) == true) && (strncmp(buf, "0::", 3) == 0))
  {
    char* nl = strchr(buf, '\n');
    if (nl != NULL)
      *nl = 0;

    char* cg = &buf[3];                                   // "/user.slice/..." or "/"

    while (true)
    {
      char val[64];

      snprintf(path, sizeof(path), "/sys/fs/cgroup%s%smemory.max", cg, (cg[strlen(cg) - 1] == '/') ? "" : "/");

      if ((fileRead(path, val, sizeof(val)) == true) && (strncmp(val, "max", 3) != 0))
      {
        uint64_t v = strtoull(val, NULL, 10);
        if ((v > 0) && ((limit == 0) || (v < limit)))
          limit = v;
      }

      char* slash = strrchr(cg, '/');
      if ((slash == NULL) || (slash == cg))
      {
        if (cg[1] == 0)
          break;                                          // "/" was the last one
        cg[1] = 0;                                        // up to the root
        continue;
      }
      *slash = 0;
    }

    return limit;
  }

  if (fileRead("/sys/fs/cgroup/memory/memory.limit_in_bytes", buf, sizeof(buf)) == true)
  {
    uint64_t v = strtoull(buf, NULL, 10);
    return (v >= (1ULL << 60)) ? 0 : v;
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// residentRead - the process's resident set, in bytes (/proc/self/statm, second field, in pages)
//
static uint64_t residentRead(void)
{
  char               buf[128];
  unsigned long long size;
  unsigned long long pages;

  if ((fileRead("/proc/self/statm", buf, sizeof(buf)) == false) || (sscanf(buf, "%llu %llu", &size, &pages) != 2))
    return 0;

  return (uint64_t) pages * (uint64_t) pageSize;
}



// -----------------------------------------------------------------------------
//
// sampler - keep 'resident' current, every 100 ms
//
static void* sampler(void* arg)
{
  struct timespec period = { 0, 100 * 1000 * 1000 };

  (void) arg;

  while (true)
  {
    atomic_store_explicit(&resident, residentRead(), memory_order_relaxed);
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

  uint64_t cgroup = cgroupLimit();

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
  atomic_store_explicit(&resident, residentRead(), memory_order_relaxed);

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

  uint64_t    rss  = atomic_load_explicit(&resident, memory_order_relaxed);
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
void memoryBudgetValues(uint64_t* budgetP, uint64_t* residentP, uint64_t* refusedP)
{
  *budgetP   = budget;
  *residentP = (budget != 0) ? atomic_load_explicit(&resident, memory_order_relaxed) : residentRead();
  *refusedP  = atomic_load_explicit(&refused, memory_order_relaxed);
}
