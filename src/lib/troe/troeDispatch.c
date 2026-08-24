//
// FILE            troeDispatch.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Per-connection defer queue. The linked list head/tail/count live in the
// per-connection corNgsild (so the worker that runs the request and the I/O
// thread that drains it post-response share one queue); the events themselves
// are allocated from the per-request kalloc arena. Each request's queue is
// drained at brokerPostResponseHook time, then reset. Memory of the events is
// reclaimed when the request arena is reset.
//

#include <stddef.h>                                   // NULL

#include "corNgsild/CorNgsild.h"                         // corNgsild (per-conn troeQ*)
#include "troe/TroeDriver.h"                          // troe, TroeEvent
#include "troe/troeDispatch.h"                        // Own interface



// -----------------------------------------------------------------------------
//
// queueAppend - append to the per-connection queue. Tail tracked for O(1).
//
static void queueAppend(const TroeEvent* evP)
{
  // Caller-allocated event, lifetime managed by request arena.
  // We mutate the next pointer in place — the queue owns linkage only.
  TroeEvent* mut = (TroeEvent*) evP;
  mut->next = NULL;

  if (corNgsild.troeQHead == NULL)
    corNgsild.troeQHead = mut;
  else
    ((TroeEvent*) corNgsild.troeQTail)->next = mut;

  corNgsild.troeQTail = mut;
  corNgsild.troeQCount++;
}



bool troeSync = false;   // --troeSync: write through instead of deferring



// -----------------------------------------------------------------------------
//
// dispatchOne - hand a single event straight to the plugin (--troeSync).
//
// Deliberately NOT "queue it and drain now": draining would also flush events
// queued earlier in the same request, which is harmless but would make the
// order of storage writes depend on the flag. One event in, one event out.
//
static void dispatchOne(const TroeEvent* evP)
{
  TroeEvent* mut = (TroeEvent*) evP;
  mut->next = NULL;

  if (troe.eventList != NULL)
    troe.eventList(mut, 1);
  else if (evP->op >= TroeOpAttrCreated)
  {
    if (troe.attrEvent != NULL)
      troe.attrEvent(evP);
  }
  else if (troe.entityEvent != NULL)
    troe.entityEvent(evP);
}



// -----------------------------------------------------------------------------
//
// troeDeferEntityEvent -
//
void troeDeferEntityEvent(const TroeEvent* evP)
{
  if (evP == NULL)
    return;

  if (troeSync)
  {
    dispatchOne(evP);
    return;
  }

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

  if (troeSync)
  {
    dispatchOne(evP);
    return;
  }

  queueAppend(evP);
}



// -----------------------------------------------------------------------------
//
// troeDispatchPending -
//
void troeDispatchPending(void)
{
  TroeEvent* head = (TroeEvent*) corNgsild.troeQHead;

  if (head == NULL)
    return;

  // No plugin? Drop the queue silently. The "none" plugin's hooks are
  // no-ops anyway — this branch only matters if no plugin loaded at all.
  if (troe.eventList != NULL)
  {
    troe.eventList(head, corNgsild.troeQCount);
  }
  else if (troe.attrEvent != NULL || troe.entityEvent != NULL)
  {
    for (TroeEvent* evP = head; evP != NULL; evP = evP->next)
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

  corNgsild.troeQHead  = NULL;
  corNgsild.troeQTail  = NULL;
  corNgsild.troeQCount = 0;
}
