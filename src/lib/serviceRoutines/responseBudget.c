//
// FILE            responseBudget.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                 // bool
#include <stdint.h>                                  // int64_t

#include "corTree/CorNode.h"                         // CorNode
#include "corNgsild/corNgsild.h"                     // corNgsild
#include "corNgsild/ldError.h"                       // ldError
#include "corNgsild/LdProblem.h"                     // LD_ERROR_TOO_MANY_RESULTS
#include "corNgsild/ldPagination.h"                  // ldPaginationLinkHeader, ldPaginationLinkHeaderAt
#include "db/DbQueryFilter.h"                        // DbQueryFilter
#include "serviceRoutines/responseBudget.h"          // Own interface



// -----------------------------------------------------------------------------
//
// responseBudgetBytes -
//
int64_t responseBudgetBytes = 0;



// -----------------------------------------------------------------------------
//
// responseBudgetRefused -
//
bool responseBudgetRefused(DbQueryFilter* filterP, CorNode* arrayP, bool wholeSet)
{
  if (filterP->budgetHit == false)
    return false;

  bool empty = (arrayP == NULL) || (arrayP->value.head == NULL);

  if ((wholeSet == false) && (empty == false))
    return false;

  long long mib = (long long) (filterP->maxBytes / (1024 * 1024));

  if (wholeSet)
    ldError(403, LD_ERROR_TOO_MANY_RESULTS, "Too Many Results",
            "this query needs every match at once (orderBy, an EntityMap or split entities) and they exceed the response size budget of %lld MiB (--maxResponseSize) - narrow the query",
            mib);
  else
    ldError(403, LD_ERROR_TOO_MANY_RESULTS, "Too Many Results",
            "the next entity alone exceeds the response size budget of %lld MiB (--maxResponseSize) - narrow the query to leave it out",
            mib);

  return true;
}



// -----------------------------------------------------------------------------
//
// responseBudgetLinkHeader -
//
void responseBudgetLinkHeader(DbQueryFilter* filterP, CorNode* arrayP, bool hasMore, int fetched)
{
  if (filterP->budgetHit == false)
  {
    if (((arrayP != NULL) && (arrayP->value.head != NULL)) || hasMore)
      ldPaginationLinkHeader(hasMore);
    return;
  }

  ldPaginationLinkHeaderAt(true, corNgsild.offset + fetched);
}



// -----------------------------------------------------------------------------
//
// responseBudgetFetched -
//
int responseBudgetFetched(CorNode* arrayP)
{
  int fetched = 0;

  if (arrayP != NULL)
  {
    for (CorNode* eP = arrayP->value.head; eP != NULL; eP = eP->next)
      ++fetched;
  }

  return fetched;
}
