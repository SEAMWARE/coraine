//
// FILE            troeDispatch.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Per-thread defer queue. Stored in a thread-local linked list whose head
// points at events allocated from the per-request kalloc arena. Each
// request's queue is drained at brokerPostResponseHook time, then reset.
// Memory of the events is reclaimed when the request arena is reset.
//

#include <stddef.h>                                   // NULL

#include "troe/TroeDriver.h"                          // troe, TroeEvent
#include "troe/troeDispatch.h"                        // Own interface



// -----------------------------------------------------------------------------
//
// Per-thread queue head + tail. Tail tracked to keep append O(1).
//
static __thread TroeEvent*  qHead = NULL;
static __thread TroeEvent*  qTail = NULL;
static __thread int         qCount = 0;



// -----------------------------------------------------------------------------
//
// queueAppend -
//
static void queueAppend(const TroeEvent* evP)
{
  // Caller-allocated event, lifetime managed by request arena.
  // We mutate the next pointer in place — the queue owns linkage only.
  TroeEvent* mut = (TroeEvent*) evP;
  mut->next = NULL;

  if (qHead == NULL)
    qHead = mut;
  else
    qTail->next = mut;

  qTail = mut;
  qCount++;
}



// -----------------------------------------------------------------------------
//
// troeDeferEntityEvent -
//
void troeDeferEntityEvent(const TroeEvent* evP)
{
  if (evP == NULL)
    return;
  queueAppend(evP);
}



// -----------------------------------------------------------------------------
//
// troeDeferAttrEvent -
//
void troeDeferAttrEvent(const TroeEvent* evP)
{
  if (evP == NULL)
    return;
  queueAppend(evP);
}



// -----------------------------------------------------------------------------
//
// troeDispatchPending -
//
void troeDispatchPending(void)
{
  if (qHead == NULL)
    return;

  // No plugin? Drop the queue silently. The "none" plugin's hooks are
  // no-ops anyway — this branch only matters if no plugin loaded at all.
  if (troe.eventList != NULL)
  {
    troe.eventList(qHead, qCount);
  }
  else if (troe.attrEvent != NULL || troe.entityEvent != NULL)
  {
    for (TroeEvent* evP = qHead; evP != NULL; evP = evP->next)
    {
      if (evP->op >= TroeOpAttrCreated)
      {
        if (troe.attrEvent != NULL)
          troe.attrEvent(evP);
      }
      else
      {
        if (troe.entityEvent != NULL)
          troe.entityEvent(evP);
      }
    }
  }

  qHead  = NULL;
  qTail  = NULL;
  qCount = 0;
}
