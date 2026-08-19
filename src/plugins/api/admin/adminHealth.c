//
// FILE            adminHealth.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                               // NULL

#include "kjson/kjBuilder.h"                      // kjObject, kjString, kjChildAdd
#include "corRest/CorRestState.h"                   // corRest

#include "api/admin/adminHealth.h"                // Own interface



// -----------------------------------------------------------------------------
//
// adminGetHealth -
//
bool adminGetHealth(void)
{
  KjNode* root = kjObject(corRest.kjsonP, NULL);

  kjChildAdd(root, kjString(corRest.kjsonP, "status", "ok"));

  corRest.out.responseTree = root;
  return true;
}
