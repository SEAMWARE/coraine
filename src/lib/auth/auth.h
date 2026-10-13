#ifndef CORAINE_AUTH_H_
#define CORAINE_AUTH_H_

//
// FILE            auth.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The broker's side of the auth plugin (plugin/AuthDriver.h, doc/authorization.md): loading it, asking it
// whether a request comes in (corRest's admit hook) and whether it may do what it asks (the pre-service
// hook), and the 401 / 403 / 503 when not. Without --auth none of it runs: no hook is set.
//
#include <stdbool.h>                                 // bool

#include "corArgs/corArgs.h"                         // CorArg
#include "corTree/CorNode.h"                         // CorNode



// -----------------------------------------------------------------------------
//
// authLoad - the plugin of --auth (a name in <plugins>/auth/, or a path); 0 on success
//
extern int authLoad(const char* name, char* errorBuf, int errorBufSize);



// -----------------------------------------------------------------------------
//
// authArgs - the loaded plugin's own options, NULL: none (or no plugin)
//
extern CorArg* authArgs(void);



// -----------------------------------------------------------------------------
//
// authPluginName - the loaded plugin's name ("fdsc"), NULL: none
//
extern const char* authPluginName(void);



// -----------------------------------------------------------------------------
//
// authInit - the plugin initialised and corRest's admit hook set; false: it refused - the broker must not
// start (fail closed). true with no plugin loaded.
//
extern bool authInit(void);



// -----------------------------------------------------------------------------
//
// authAuthorize - may the request do what it asks: the pre-service hook's, once the @context is resolved
// and BEFORE the tenant is (resolving it may create it). false: the error is set (403 / 503).
//
extern bool authAuthorize(void);



// -----------------------------------------------------------------------------
//
// authEntityWanted - does the plugin decide per entity (ABI 2's entity, and a request that is not the
// broker's own) - a caller that would have to look an entity up only for the check asks this first
//
extern bool authEntityWanted(void);



// -----------------------------------------------------------------------------
//
// authEntityAllowed / authEntityTreeAllowed - may the bound request read or write this entity: its
// EXPANDED types (stored: the stored entity's; false: as the request sends them), the expanded attributes
// it touches (NULL: the whole entity). false: *detailP says why (in the request's arena). true when the
// plugin does not decide per entity. The Tree variant takes the types as a tree: "T" or ["T1", "T2"].
//
extern bool authEntityAllowed(const char* id, const char* const* typeV, int types, bool stored, const char* const* attrV, int attrs, const char** detailP);
extern bool authEntityTreeAllowed(const char* id, CorNode* typeP, bool stored, const char** detailP);



// -----------------------------------------------------------------------------
//
// authEntityStoredCheck - the request's one entity, by id, its stored types looked up - and the types the
// request body gives it, if it gives any: may it. false: the 403 is set - the service routine returns. Call it first thing, before anything is forwarded (distributed
// operations), handed to a bridge or written. Costs nothing when the plugin does not decide per entity.
//
extern bool authEntityStoredCheck(const char* id);



// -----------------------------------------------------------------------------
//
// authEntityBatchAllowed - one entity of a batch: lookup - its stored types when it exists (an upsert, an
// update, a merge, a delete) AND the types the body gives it (bodyTypeP, NULL: none) - a write that changes
// or adds a type needs the right to it; an entity that does not exist (or no lookup: a create) - bodyTypeP.
// false: *detailP says why - the entity's error in the 207.
//
extern bool authEntityBatchAllowed(const char* id, CorNode* entryP, bool lookup, const char** detailP);



// -----------------------------------------------------------------------------
//
// authEntityArrayFilter - a query's entities, as they go into the response: each the request may not read
// is left out (a page may hold fewer than the limit; a count counts it). false: join=inline - refused (403
// set), its nested entities are not asked about.
//
extern bool authEntityArrayFilter(CorNode* arrayP);



// -----------------------------------------------------------------------------
//
// authEntityAttrsFilter - a read's entity (its attributes at the top, expanded): false - it may not be read; true
// - it may, and the attributes no policy allows (ngsi-ld:attribute, AuthAnswer.partial) are taken out of it
//
extern bool authEntityAttrsFilter(CorNode* entityP);



// -----------------------------------------------------------------------------
//
// authEntityDeny - the request's answer when its one entity may not: 403 with that detail
//
extern void authEntityDeny(const char* detail);



// -----------------------------------------------------------------------------
//
// authSubscriptionOwnerKeep / authSubscriptionOwnerForget - a subscription's owner (who made it, or last
// updated it): kept, in the store, beside the subscription - for its notifications, decided entity by
// entity for that owner (fail closed: no owner known, no entity). Keep after a create and an update,
// forget after a delete. Nothing without an auth plugin that keeps identities.
//
extern void authSubscriptionOwnerKeep(const char* subId);
extern void authSubscriptionOwnerForget(const char* subId);



// -----------------------------------------------------------------------------
//
// authRelease - the request is over: its identity back to the plugin (the post-response hook)
//
extern void authRelease(void);



// -----------------------------------------------------------------------------
//
// authTrustNext - the next request THIS thread runs is the broker's own (corRestRunJson, e.g. deleting a
// closed WebSocket's subscriptions): let in without asking. Consumed by that request's admission - call
// it immediately before the corRestRunJson.
//
extern void authTrustNext(void);



// -----------------------------------------------------------------------------
//
// authTrustClear - authTrustNext's mark dropped, consumed or not - call it right after the corRestRunJson:
// a run that never reached admission must not leave the next request on this thread trusted
//
extern void authTrustClear(void);



// -----------------------------------------------------------------------------
//
// authVersionInfo - the plugin's line for GET /version, NULL: no plugin
//
extern const char* authVersionInfo(void);



// -----------------------------------------------------------------------------
//
// authClose - the broker stops
//
extern void authClose(void);

#endif  // CORAINE_AUTH_H_
