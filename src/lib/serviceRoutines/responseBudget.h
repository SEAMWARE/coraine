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
// (an orderBy query orders the whole set before paginating it, a split-entity assembly
// without an EntityMap merges it) and a part of it is a wrong answer. An EntityMap is
// not one of them: it freezes the ids of the set, and its pages are slices of it.
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




// -----------------------------------------------------------------------------
//
// responseBudgetTooMany - 403 TooManyResults: `what` does not fit in the response size budget
//
// For the cases responseBudgetRefused does not see - a temporal query, a distributed one. `what`
// names it ("the first entity's instances, one per attribute", ...); the detail adds the budget.
//
extern void responseBudgetTooMany(const char* what);



// -----------------------------------------------------------------------------
//
// responseBudgetDepth - how deep the parts of one page can go, all of them, within the budget
//
// A page assembled from several parts - the local store's page and each Context Source's, or the
// instance arrays of one temporal entity - that are each a prefix of a sequence sharing one paging
// parameter (offset; offsetN). Cutting every part at the same depth d keeps the page exact: positions
// [0, d) of every part are in it, none after, and offset + d (offsetN + d) is where every part's next
// page starts. Cutting one part alone would lose (or repeat) the others' elements on the next page.
//
// Returns the largest d at which every part cut to its first d elements is at most `budget` bytes
// (an element measured as rendered, corJsonFastRenderSize) - or -1 when the parts fit whole and
// nothing needs cutting. Further bounds on d, applied only when a cut is needed:
//   maxCount  > 0: the elements of all parts together at most maxCount (limit) - so the page needs
//                  no trim after the cut, and the cut is where it ends;
//   maxDepth >= 0: d at most maxDepth - a part that already stopped there (a store's page that the
//                  budget ended: its elements after maxDepth were never fetched). A part longer
//                  than maxDepth is then a cut too, even within the budget - and so is a page of
//                  more than maxCount elements.
// 0: not even the first elements fit together - nothing can be returned.
//
extern int responseBudgetDepth(CorNode** partV, int parts, int64_t budget, int maxCount, int maxDepth);



// -----------------------------------------------------------------------------
//
// responseBudgetCut - cut every part (an array) to its first `depth` elements
//
extern void responseBudgetCut(CorNode** partV, int parts, int depth);

#endif  // SRC_LIB_SERVICEROUTINES_RESPONSEBUDGET_H_
