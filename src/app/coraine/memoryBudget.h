#ifndef SRC_APP_CORAINE_MEMORYBUDGET_H_
#define SRC_APP_CORAINE_MEMORYBUDGET_H_

//
// FILE            memoryBudget.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                   // bool
#include <stdint.h>                                    // uint64_t



// -----------------------------------------------------------------------------
//
// memoryBudgetInit - refuse requests rather than be killed for memory
//
// In a container with a memory limit the kernel kills the process that goes over it - SIGKILL, exit
// 137, no warning, and with corDB the whole store with it. So the broker keeps a budget below that
// limit and turns requests away (503 + Retry-After) as it gets close:
//
//   budget  --memoryLimit (MiB), else 85% of the cgroup's memory.max, else none (nothing is checked)
//   soft    90% of the budget - writes that grow memory (POST, PUT, PATCH) are refused
//   hard    the budget        - everything is refused except what frees memory or reports on the
//                               broker: DELETE, batch delete, /version, /metrics, /admin/*
//
// Usage is the process's resident set, read from /proc/self/statm by a thread of its own every
// 100 ms - so it counts every library (mongoc, MHD, OpenSSL ...) and costs a request one load and a
// compare. Not a check in each malloc: a library handed NULL from malloc does not fail gracefully.
//
// Over the soft limit the same thread calls malloc_trim(0), at most every 2 s: glibc keeps freed memory
// resident on its free lists, so without it the resident set stays at its high-water mark after the
// requests that pushed it there are long gone.
//
extern void memoryBudgetInit(int limitMiB);



// -----------------------------------------------------------------------------
//
// memoryBudgetAdmit - may this request go on? false = refused, the 503 already set
//
extern bool memoryBudgetAdmit(void);



// -----------------------------------------------------------------------------
//
// memoryBudgetValues - the budget (0: none), the resident set and how many requests were refused
//
extern void memoryBudgetValues(uint64_t* budgetP, uint64_t* residentP, uint64_t* refusedP);

#endif  // SRC_APP_CORAINE_MEMORYBUDGET_H_
