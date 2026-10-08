#ifndef SRC_LIB_SERVICEROUTINES_RESPONSEBUDGET_H_
#define SRC_LIB_SERVICEROUTINES_RESPONSEBUDGET_H_

//
// FILE            responseBudget.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                 // bool
#include <stdint.h>                                  // int64_t

#include "corTree/CorNode.h"                         // CorNode
#include "db/DbQueryFilter.h"                        // DbQueryFilter



// -----------------------------------------------------------------------------
//
// responseBudgetBytes - the byte budget of an entity query (--maxResponseSize, in bytes; 0 = none)
//
// Handed to the store plugin as DbQueryFilter.maxBytes: the plugin stops fetching
// before the entity that would take the page past it (DbQueryFilter.h).
//
extern int64_t responseBudgetBytes;



// -----------------------------------------------------------------------------
//
// responseBudgetRefused - after a fetch with a byte budget: answer 403 TooManyResults if it must be refused
//
// A short page is an answer only where a page is: when the store paginates and at
// least one entity fitted. Refused (and true returned) when the budget was spent
// and either nothing fitted at all, or wholeSet - the caller needs every match
// (an orderBy query orders the whole set before paginating it, an EntityMap
// freezes it, a split-entity assembly merges it) and a part of it is a wrong answer.
//
extern bool responseBudgetRefused(DbQueryFilter* filterP, CorNode* arrayP, bool wholeSet);



// -----------------------------------------------------------------------------
//
// responseBudgetLinkHeader - the pagination Link header of a page that the byte budget may have ended
//
// hasMore is what ldPaginationTrim said. A page the budget ended has more after
// it by construction (the entity that did not fit), and the next page starts
// where the store stopped - offset + fetched, the entities the store returned
// (counted before anything is filtered out of the page: offset counts the
// store's positions) - not `limit` further on.
//
extern void responseBudgetLinkHeader(DbQueryFilter* filterP, CorNode* arrayP, bool hasMore, int fetched);



// -----------------------------------------------------------------------------
//
// responseBudgetFetched - how many entities the store returned (the array's length)
//
extern int responseBudgetFetched(CorNode* arrayP);

#endif  // SRC_LIB_SERVICEROUTINES_RESPONSEBUDGET_H_
