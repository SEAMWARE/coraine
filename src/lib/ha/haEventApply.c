//
// FILE            haEventApply.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                      // NULL

#include "kalloc/kalloc.h"                               // kaBufferInit, kaBufferReset
#include "kjson/kjson.h"                                 // Kjson
#include "kjson/kjBufferCreate.h"                        // kjBufferCreate
#include "ktrace/kTrace.h"                               // KT_*

#include "swRest/SwRestState.h"                          // swRest
#include "swNgsild/SwNgsild.h"                           // swNgsild

#include "db/Tenant.h"                                   // tenantSubCacheItemRefresh, ...
#include "db/contextCache.h"                             // contextCacheItemRefresh, contextCacheItemDrop
#include "ha/haInit.h"                                   // haApplyWait
#include "swBrokerTraceLevels.h"                         // KtHa
#include "ha/haEventApply.h"                             // Own interface



// -----------------------------------------------------------------------------
//
// stateBind - bring this thread's request state up to working order
//
// A channel runs in a thread of its own, with no request behind it, and the DB
// driver allocates what it reads through swRest.kalloc/kjsonP. Both are __thread
// (via the per-connection fallback), so the channel thread gets its own - zeroed
// until somebody sets them up, which is what this does, once.
//
// The buffer is reset per event: everything an apply keeps has been cloned into
// the cache's own malloc'd storage by then, so nothing outside this call holds a
// pointer into it.
//
static void stateBind(Tenant* tenantP)
{
  static __thread bool inited = false;

  if (inited == false)
  {
    kaBufferInit(&swRest.kalloc, swRest.kallocBuffer, sizeof(swRest.kallocBuffer), 256 * 1024, NULL, "ha");
    swRest.kjsonP = kjBufferCreate(&swRest.kjson, &swRest.kalloc);
    inited = true;
  }
  else
    //
    // ⚠️ KTRUE = REUSE, and it is not optional. kaBufferReset(kaP, KFALSE) frees
    // every extra block but leaves kaP->allocList pointing at them - it is the
    // TEARDOWN call. Reaching it a second time on the same arena walks that
    // dangling list and frees the same pointers again: "double free or
    // corruption", in whatever thread happens to be there.
    //
    kaBufferReset(&swRest.kalloc, KTRUE);

  //
  // The apply runs AS the event's tenant. Not everything downstream takes the
  // tenant as a parameter, and a thread with no request has none.
  //
  swNgsild.tenantP = tenantP;
}



// -----------------------------------------------------------------------------
//
// haEventApply -
//
// ⚠️ THE INVARIANT OF THIS PATH: what arrives here HAS ALREADY HAPPENED
// somewhere else. An apply reads, and only reads. It must never write the change
// back - the instance that made it shares the database - and it must never go to
// the network to complete it. Anything the item references is itself an item
// somebody persisted, and that persist raises its own event: a reference is not
// something to go and fetch, it is something that arrives.
//
// ⚠️⚠️ AND IT NEVER NOTIFIES. An initial notification (§ 5.11.2.4 - a CSR
// subscription is answered with the registrations that already match it, and a
// PATCH that widens one is answered again) belongs to the instance the REQUEST
// landed on, and to that instance alone. Every other broker learns the same
// subscription over HA a few milliseconds later, and if applying it notified,
// a subscriber behind five brokers would get five copies of every initial
// notification - and five more on every PATCH.
//
// It holds by construction, and that is where it has to keep holding: notifying
// is done by the SERVICE ROUTINES (postCsourceSubscriptions and
// patchCsourceSubscription call ldCsrSubInitialNotify), never by the cache
// functions this path calls. ⇒ AN APPLY MAY ONLY EVER CALL SOMETHING THE
// STARTUP CACHE LOAD ALSO CALLS - the startup load has exactly the same
// prohibition, for exactly the same reason.
//
bool haEventApply(HaEvent* eventP)
{
  //
  // The channel should have waited before it even resolved the tenant - this is
  // the backstop for one that did not, and once the caches are loaded it is a
  // single bool read.
  //
  haApplyWait();

  if (eventP->id == NULL)
    return false;

  if ((eventP->kind != HaContext) && (eventP->tenantP == NULL))
    return false;

  KT_T(KtHa, "applying %s of %s '%s' (tenant '%s')",
       (eventP->op == HaOpDelete)? "a delete" : "an upsert",
       (eventP->kind == HaSubscription)? "subscription" : (eventP->kind == HaRegistration)? "registration" : "@context",
       eventP->id,
       (eventP->tenantP != NULL && eventP->tenantP->name[0])? eventP->tenantP->name : "(default)");

  //
  // A channel that carries the item itself (haaux) hands it over in apiP, and
  // the apply then has nothing to read. Not written yet - and answering "applied"
  // to an event we have not applied is worse than refusing it.
  //
  if (eventP->apiP != NULL)
  {
    KT_E("HA: '%s' arrived with a payload - not implemented yet (haaux)", eventP->id);
    return false;
  }

  stateBind(eventP->tenantP);

  //
  // A delete of something we never had in the cache is not a failure - it is the
  // state the event asked for. Only a read that failed is.
  //
  bool ok = true;

  if (eventP->op == HaOpDelete)
  {
    switch (eventP->kind)
    {
    case HaSubscription:  tenantSubCacheItemDrop(eventP->tenantP, eventP->id);  break;
    case HaRegistration:  tenantRegCacheItemDrop(eventP->tenantP, eventP->id);  break;
    case HaContext:       contextCacheItemDrop(eventP->id);                     break;
    }
  }
  else
  {
    switch (eventP->kind)
    {
    case HaSubscription:  ok = tenantSubCacheItemRefresh(eventP->tenantP, eventP->id);  break;
    case HaRegistration:  ok = tenantRegCacheItemRefresh(eventP->tenantP, eventP->id);  break;
    case HaContext:       ok = contextCacheItemRefresh(eventP->id);                     break;
    }
  }

  if (ok == false)
    KT_W("HA: could not apply the change to '%s'", eventP->id);

  return ok;
}
