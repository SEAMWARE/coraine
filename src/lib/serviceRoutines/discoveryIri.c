//
// FILE            discoveryIri.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                   // NULL
#include <string.h>                                   // strchr

#include "corJsonld/CorLdContext.h"                   // CorLdContext
#include "corJsonld/CorLdItem.h"                      // CorLdItem
#include "corJsonld/corLdExpand.h"                    // contextItemLookup
#include "corJsonld/corLdInit.h"                      // corLdCorePristine

#include "serviceRoutines/discoveryIri.h"             // Own interface



// -----------------------------------------------------------------------------
//
// discoveryIri -
//
const char* discoveryIri(const char* name)
{
  //
  // An IRI already (every name that is not a core term is held expanded) - nothing to look up
  //
  if ((name == NULL) || (strchr(name, ':') != NULL))
    return name;

  CorLdContext* pristineP = corLdCorePristine();
  if (pristineP == NULL)
    return name;

  CorLdItem* itemP = contextItemLookup(pristineP, name);
  if ((itemP == NULL) || (itemP->id == NULL) || (strchr(itemP->id, ':') == NULL))
    return name;

  return itemP->id;
}
