//
// FILE            adminHealth.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                               // NULL

#include "kjson/kjBuilder.h"                      // kjObject, kjString, kjChildAdd
#include "swRest/SwRestState.h"                   // swRest

#include "api/admin/adminHealth.h"                // Own interface



// -----------------------------------------------------------------------------
//
// adminGetHealth -
//
bool adminGetHealth(void)
{
  KjNode* root = kjObject(swRest.kjsonP, NULL);

  kjChildAdd(root, kjString(swRest.kjsonP, "status", "ok"));

  swRest.out.responseTree = root;
  return true;
}
