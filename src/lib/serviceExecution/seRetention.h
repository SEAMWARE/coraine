#ifndef SRC_LIB_SERVICEEXECUTION_SERETENTION_H_
#define SRC_LIB_SERVICEEXECUTION_SERETENTION_H_

//
// FILE            seRetention.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// How many ended Service Executions are kept: --executionMaxEnded per tenant, the oldest to end going first
// (the time limit, --executionRetention, is the sweep's - seExecution.c). Only TOP-LEVEL executions count:
// a child of a combined or grouped one goes with its parent, never before it.
//
// The ring holds ids, in the order they ended. A push of the one past the cap hands back the oldest - the
// caller deletes it (and its tree). An id deleted meanwhile (DELETE /services/{id}, the sweep) stays in the
// ring until its turn, and its deletion then finds nothing.
//
#include <stdbool.h>                                  // bool

#include "db/Tenant.h"                                // Tenant



// -----------------------------------------------------------------------------
//
// seRetentionMaxEnded - the cap (0: none)
//
extern int seRetentionMaxEnded;



// -----------------------------------------------------------------------------
//
// seRetentionPush - an execution of this tenant ended; returns the id to delete now (malloc - the caller
// frees it), or NULL
//
extern char* seRetentionPush(Tenant* tenantP, const char* execId);



// -----------------------------------------------------------------------------
//
// seRetentionSeeded - has the tenant's ring been filled with what was stored before the broker started?
// The first call answers false and marks it: the caller then pushes those, oldest first.
//
extern bool seRetentionSeeded(Tenant* tenantP);

#endif  // SRC_LIB_SERVICEEXECUTION_SERETENTION_H_
