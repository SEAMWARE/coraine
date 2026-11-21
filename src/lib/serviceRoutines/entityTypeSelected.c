//
// FILE            entityTypeSelected.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                 // bool
#include <stddef.h>                                  // NULL

#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeLookup.h"                   // corTreeLookup

#include "corNgsild/corNgsild.h"                     // corNgsild
#include "corNgsild/ldEntityMatch.h"                 // ldEntityMatchType

#include "serviceRoutines/entityTypeSelected.h"      // Own interface



// -----------------------------------------------------------------------------
//
// entityTypeSelected -
//
// The write operations on one Entity (TS 104-175 § 10.2.3 - § 10.2.11) find it only where "id (URI), and where
// specified type, is equivalent": a ?type naming none of its types makes the Entity not found, like an unknown id.
//
bool entityTypeSelected(CorNode* entityP)
{
  if ((corNgsild.typeExpr == NULL) || (entityP == NULL))
    return true;

  return ldEntityMatchType(corTreeLookup(entityP, "type"), corNgsild.typeExpr);
}
