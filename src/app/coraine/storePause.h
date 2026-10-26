#ifndef SRC_APP_CORAINE_STOREPAUSE_H_
#define SRC_APP_CORAINE_STOREPAUSE_H_

//
// FILE            storePause.h
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
// storePauseAdmit - may this request go on while the store is paused? false = refused, the 503 already set
//
// A store that cannot keep a write - corDB with a log it cannot write to its disk - pauses (DbDriver.h,
// DbPausedFunc), and ends the pause itself once it can keep writes again. While it is paused, every
// request is refused with 503 PersistenceUnavailable and Retry-After, before anything is parsed or
// applied, except the ones that write nothing:
//
//   GET, HEAD, OPTIONS
//   POST /ngsi-ld/v1/entityOperations/query, /temporal/entityOperations/query,
//        /temporal/timepoint/entityOperations/query
//   /admin/log - the log and trace levels
//
// DELETE is refused too: a delete is a record in the log as much as a create is. Not paused (or a store
// that never pauses): one atomic load.
//
extern bool storePauseAdmit(void);



// -----------------------------------------------------------------------------
//
// storePauseValues - is the store paused, since when (ns since the epoch), why (reasonLen bytes), and how
// many requests were refused for it since the start
//
extern bool storePauseValues(uint64_t* sinceNsP, char* reason, int reasonLen, uint64_t* refusedP);

#endif  // SRC_APP_CORAINE_STOREPAUSE_H_
