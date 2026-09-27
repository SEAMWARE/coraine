//
// FILE            adminHealth.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                               // NULL

#include "corTree/corTreeBuilder.h"               // corTreeObject, corTreeString, corTreeChildAdd
#include "corRest/CorRestState.h"                   // corRest

#include "api/admin/adminHealth.h"                // Own interface



// -----------------------------------------------------------------------------
//
// adminGetHealth -
//
bool adminGetHealth(void)
{
  CorNode* root = corTreeObject(corRest.kallocP, NULL);

  corTreeChildAdd(root, corTreeString(corRest.kallocP, "status", "ok"));

  corRest.out.responseTree = root;
  return true;
}
