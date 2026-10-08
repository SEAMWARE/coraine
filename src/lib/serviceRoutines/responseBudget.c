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
#include "corJson/corJsonRenderSize.h"               // corJsonFastRenderSize
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



// -----------------------------------------------------------------------------
//
// responseBudgetTooMany -
//
void responseBudgetTooMany(const char* what)
{
  long long mib = (long long) (responseBudgetBytes / (1024 * 1024));

  ldError(403, LD_ERROR_TOO_MANY_RESULTS, "Too Many Results",
          "%s exceeds the response size budget of %lld MiB (--maxResponseSize) - narrow the query",
          what, mib);
}



// -----------------------------------------------------------------------------
//
// responseBudgetDepth -
//
int responseBudgetDepth(CorNode** partV, int parts, int64_t budget, int maxCount, int maxDepth)
{
  if (budget <= 0)
    return -1;

  CorNode* cursorV[parts];                           // the element at the current depth, of each part
  int      maxLen = 0;

  for (int p = 0; p < parts; p++)
  {
    cursorV[p] = ((partV[p] != NULL) && (partV[p]->type == CorArray)) ? partV[p]->value.head : NULL;

    int len = 0;
    for (CorNode* eP = cursorV[p]; eP != NULL; eP = eP->next)
      ++len;
    if (len > maxLen)
      maxLen = len;
  }

  int64_t bytes      = 0;
  int     count      = 0;
  int     countDepth = -1;                           // the first depth whose elements pass maxCount
  int     cut        = -1;

  for (int d = 0; d < maxLen; d++)
  {
    if ((maxDepth >= 0) && (d >= maxDepth))          // a part stopped here: the others stop with it
    {
      cut = d;
      break;
    }

    int64_t layerBytes = 0;
    int     layerCount = 0;

    for (int p = 0; p < parts; p++)
    {
      if (cursorV[p] == NULL)
        continue;

      layerBytes += corJsonFastRenderSize(cursorV[p]);
      layerCount += 1;
      cursorV[p]  = cursorV[p]->next;
    }

    if ((maxCount > 0) && (countDepth < 0) && (count + layerCount > maxCount))
      countDepth = d;

    if (bytes + layerBytes > budget)
    {
      cut = d;
      break;
    }

    bytes += layerBytes;
    count += layerCount;
  }

  //
  // No cut for the budget: none at all - unless a part stopped early (maxDepth), whose page is then
  // already one the budget ended, and its next link is offset + a depth: the elements of all parts
  // must then fit in maxCount too, or the trim to limit would drop some of them before that depth
  //
  //
  // A countDepth of 0 - more parts than maxCount - fits no depth at all; the page is then cut by the
  // trim to limit as without a budget, rather than refused for a count the client chose.
  //
  if (cut < 0)
    return ((maxDepth >= 0) && (countDepth > 0)) ? countDepth : -1;

  if ((countDepth > 0) && (countDepth < cut))
    cut = countDepth;

  return cut;
}



// -----------------------------------------------------------------------------
//
// responseBudgetCut -
//
void responseBudgetCut(CorNode** partV, int parts, int depth)
{
  for (int p = 0; p < parts; p++)
  {
    CorNode* arrayP = partV[p];

    if ((arrayP == NULL) || (arrayP->type != CorArray))
      continue;

    if (depth == 0)
    {
      arrayP->value.head = NULL;
      arrayP->value.tail = NULL;
      continue;
    }

    CorNode* eP = arrayP->value.head;
    for (int d = 1; (eP != NULL) && (d < depth); d++)
      eP = eP->next;

    if (eP != NULL)
    {
      eP->next           = NULL;
      arrayP->value.tail = eP;
    }
  }
}
