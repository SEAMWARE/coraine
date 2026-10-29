//
// FILE            adminProbes.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdatomic.h>                            // atomic_load_explicit
#include <stddef.h>                               // NULL
#include <string.h>                               // strcmp

#include "corAlloc/CorAlloc.h"                    // CorAlloc
#include "corTree/corTreeBuilder.h"               // corTreeObject, corTreeString, corTreeChildAdd, corTreeChildAddSorted
#include "corRest/CorRestState.h"                 // corRest
#include "corNgsild/corNgsild.h"                  // corNgsild, ldError, LD_ERROR_*

#include "probe/probe.h"                          // Probe, probeList, probeReset

#include "api/admin/adminProbes.h"                // Own interface



// -----------------------------------------------------------------------------
//
// adminGetProbes - the probes sorted by name: the response is the same whichever order they were first set in
//
bool adminGetProbes(void)
{
  CorAlloc* allocP = corRest.kallocP;
  CorNode*  root   = corTreeObject(allocP, NULL);

  for (Probe* probeP = probeList(); probeP != NULL; probeP = probeP->next)
  {
    const char* value = atomic_load_explicit(&probeP->value, memory_order_acquire);

    if (value != NULL)
      corTreeChildAddSorted(root, corTreeString(allocP, probeP->name, value));
  }

  corRest.out.responseTree = root;
  corNgsild.rawResponse    = true;
  return true;
}



// -----------------------------------------------------------------------------
//
// adminGetProbe -
//
bool adminGetProbe(void)
{
  const char* name = corRest.in.wildcard[0];

  for (Probe* probeP = probeList(); probeP != NULL; probeP = probeP->next)
  {
    if (strcmp(probeP->name, name) != 0)
      continue;

    const char* value = atomic_load_explicit(&probeP->value, memory_order_acquire);

    if (value == NULL)
      break;

    CorNode* root = corTreeObject(corRest.kallocP, NULL);

    corTreeChildAdd(root, corTreeString(corRest.kallocP, probeP->name, value));
    corRest.out.responseTree = root;
    corNgsild.rawResponse    = true;
    return true;
  }

  ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Probe Not Set", "%s", name);
  return true;
}



// -----------------------------------------------------------------------------
//
// adminDeleteProbes -
//
bool adminDeleteProbes(void)
{
  probeReset();
  corRest.out.httpStatusCode = 204;
  return true;
}
