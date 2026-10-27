//
// FILE            distSubPersist.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                  // NULL

#include "corRest/CorRestState.h"                      // corRest
#include "corTree/CorNode.h"                           // CorNode
#include "corNgsild/LdSubCache.h"                     // LdSubCacheItem
#include "corNgsild/ldDistSub.h"                      // ldDistSubSubordinatesFragment

#include "db/DbDriver.h"                             // db
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/distSubPersist.h"          // Own interface



// -----------------------------------------------------------------------------
//
// distSubPersist -
//
void distSubPersist(LdSubCacheItem* itemP, void* userData)
{
  if (itemP == NULL || itemP->subId == NULL || db.subscriptionUpdate == NULL)
    return;

  Tenant*  tP    = (Tenant*) userData;
  CorNode* fragP = ldDistSubSubordinatesFragment(itemP, corRest.kallocP);
  if (fragP == NULL)
    return;

  db.subscriptionUpdate(tP, itemP->subId, fragP);
}
