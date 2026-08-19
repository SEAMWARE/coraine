//
// FILE            dbExpiredEntities.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Lazy expiry of transient Entities (§ 5.2.4).
//
// The broker runs no clean-up thread. An expired Entity is noticed by whatever
// read happens to walk over it: that read drops it from the response and calls
// dbExpiredEntityDefer, and the delete happens in the post-response hook.
//
// Two consequences worth stating, since they are deliberate:
//   - An Entity nobody reads is never deleted. It is also never served, so it
//     is invisible rather than wrong.
//   - The read paths must NOT push an expiresAt predicate into the database.
//     The expired rows have to reach RAM for the broker to notice them at all;
//     filtering them away in SQL/mongo would hide exactly what we came for.
//
// The queue lives in the per-connection corNgsild state, NOT in a thread-local:
// the post-response hook does not necessarily run on the thread that served the
// request, so a __thread queue is simply empty by the time it drains. Same
// placement as the notification and CSR-probe queues next to it.
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp

#include "ktrace/kTrace.h"                           // KT_E, KT_T
#include "kalloc/kaStrdup.h"                         // kaStrdup

#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjBuilder.h"                          // kjChildRemove

#include "corRest/CorRestState.h"                      // corRest
#include "corNgsild/corNgsild.h"                       // corNgsild
#include "corNgsild/ldDistMerge.h"                    // ldDistInstanceIsExpired

#include "db/DbDriver.h"                             // db
#include "db/Tenant.h"                               // Tenant
#include "db/dbExpiredEntities.h"                    // Own interface



// -----------------------------------------------------------------------------
//
// dbExpiredEntityDefer -
//
void dbExpiredEntityDefer(Tenant* tenantP, const char* entityId)
{
  if (tenantP == NULL || entityId == NULL || entityId[0] == 0)
    return;

  // A query can walk over the same Entity twice (split sources, pagination
  // re-reads). Deleting once is enough and the second DELETE would only log.
  for (int i = 0; i < corNgsild.expiredN; i++)
  {
    if ((corNgsild.expiredV[i].tenantP == tenantP) && (strcmp(corNgsild.expiredV[i].entityId, entityId) == 0))
      return;
  }

  // Beyond the cap the Entity simply stays until some later read finds it —
  // the whole mechanism is opportunistic, so dropping a few is harmless.
  if (corNgsild.expiredN >= LD_EXPIRED_PENDING_MAX)
    return;

  corNgsild.expiredV[corNgsild.expiredN].tenantP  = tenantP;
  corNgsild.expiredV[corNgsild.expiredN].entityId = kaStrdup(&corRest.kalloc, entityId);
  corNgsild.expiredN++;
}



// -----------------------------------------------------------------------------
//
// dbExpiredEntityDispatchPending -
//
void dbExpiredEntityDispatchPending(void)
{
  if (corNgsild.expiredN == 0)
    return;

  for (int i = 0; i < corNgsild.expiredN; i++)
  {
    if (db.entityDelete == NULL)
      break;

    int r = db.entityDelete((Tenant*) corNgsild.expiredV[i].tenantP, corNgsild.expiredV[i].entityId);

    // Not an error worth escalating: the response is already sent, and a
    // concurrent DELETE beating us here is a perfectly ordinary race.
    if (r != DB_OK)
      KT_T(LdTExpiry, "expired entity '%s' not removed (%d)", corNgsild.expiredV[i].entityId, r);
    else
      KT_T(LdTExpiry, "expired entity '%s' removed", corNgsild.expiredV[i].entityId);
  }

  corNgsild.expiredN = 0;
}



// -----------------------------------------------------------------------------
//
// dbExpiredEntityIs -
//
bool dbExpiredEntityIs(Tenant* tenantP, KjNode* entityP)
{
  if ((entityP == NULL) || (entityP->type != KjObject))
    return false;

  // ldDistInstanceIsExpired reads expiresAt off a node in either form
  // (nanosecond integer or ISO string) — an Entity's top-level expiresAt is
  // the same lookup as an Attribute instance's.
  if (!ldDistInstanceIsExpired(entityP, (int64_t) corRest.requestStartTime))
    return false;

  KjNode* idP = kjLookup(entityP, "id");
  if ((idP != NULL) && (idP->type == KjString))
    dbExpiredEntityDefer(tenantP, idP->value.s);

  return true;
}



// -----------------------------------------------------------------------------
//
// dbExpiredEntityFilter -
//
void dbExpiredEntityFilter(Tenant* tenantP, KjNode* arrayP)
{
  if ((arrayP == NULL) || (arrayP->type != KjArray))
    return;

  KjNode* eP = arrayP->value.firstChildP;

  while (eP != NULL)
  {
    KjNode* nextP = eP->next;

    if (dbExpiredEntityIs(tenantP, eP))
      kjChildRemove(arrayP, eP);

    eP = nextP;
  }
}
