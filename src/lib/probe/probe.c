//
// FILE            probe.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Probes - see probe.h
//
#include <stdatomic.h>                             // atomic_*
#include <stdbool.h>                               // false
#include <stddef.h>                                // NULL

#include "probe/probe.h"                           // Own interface



// -----------------------------------------------------------------------------
//
// probeHead - the registered probes; a probe is added at the head and never removed
//
static _Atomic(Probe*) probeHead = NULL;



// -----------------------------------------------------------------------------
//
// probeSet -
//
// The value is stored before the probe is added to the list: a reader that finds it there finds it set.
//
void probeSet(Probe* probeP, const char* value)
{
  atomic_store_explicit(&probeP->value, value, memory_order_release);

  if (atomic_flag_test_and_set_explicit(&probeP->registered, memory_order_relaxed) == false)
  {
    Probe* head = atomic_load_explicit(&probeHead, memory_order_relaxed);

    do
    {
      probeP->next = head;
    } while (atomic_compare_exchange_weak_explicit(&probeHead, &head, probeP, memory_order_release, memory_order_relaxed) == false);
  }
}



// -----------------------------------------------------------------------------
//
// probeList -
//
Probe* probeList(void)
{
  return atomic_load_explicit(&probeHead, memory_order_acquire);
}



// -----------------------------------------------------------------------------
//
// probeReset -
//
void probeReset(void)
{
  for (Probe* probeP = probeList(); probeP != NULL; probeP = probeP->next)
    atomic_store_explicit(&probeP->value, NULL, memory_order_relaxed);
}
