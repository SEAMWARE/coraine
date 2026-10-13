#ifndef CORAINE_AUTHDRIVER_H_
#define CORAINE_AUTHDRIVER_H_

//
// FILE            AuthDriver.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// An AUTH plugin decides who a request comes from and whether it may do what it asks
// (doc/authorization.md). One at most (--auth <name>, <plugins>/auth/<name>.so); without one the broker
// lets every request in, as it always has, and none of this code runs.
//
// The broker asks twice per request:
//   1. authenticate - right after the route is looked up, BEFORE anything is done for the request: no
//      body parsed, no @context downloaded. Who is it? AuthUnauthenticated is a 401.
//   2. authorize    - once the tenant and the @context are resolved, before the service routine. May
//      it? AuthDeny is a 403.
// Every request - HTTP, cor:// and each message of a transport (WebSocket) - is asked, each with its own
// headers. A request the broker makes to itself (an in-process forward) is not: it was let in when it
// arrived.
//
// Everything that crosses is plain data - const char*, integers - as for a bridge (corBridge): a plugin
// need not know the broker's trees or allocators, and need not be written in C. Strings the broker hands
// over are valid for the call only; strings the plugin answers with are the plugin's (static, or alive
// until release).
//
// ⚠ Called on the broker's request threads, concurrently and - with coroutines - possibly on a
// coroutine's stack: a plugin is thread-safe and NEVER blocks (no network I/O on a request's path - keys
// and policies are fetched by threads of its own, in the background).
//
// The plugin exports one function, authRegister(AuthDriver*), and fills in the struct. The struct is the
// broker's, allocated at the size of ITS header: abiVersion says how much room there is before the plugin
// writes - append-only, as for bridges and transports.
//
#include <stdbool.h>                                 // bool
#include <stdint.h>                                  // uint64_t

#include "corArgs/corArgs.h"                         // CorArg



#define AUTH_ABI_VERSION  3



// -----------------------------------------------------------------------------
//
// AuthVerdict - a plugin's answer
//
typedef enum AuthVerdict
{
  AuthAllow           = 0,   // go on
  AuthUnauthenticated = 1,   // 401 - no credentials, or credentials that do not hold
  AuthDeny            = 2,   // 403 - who it is is known, and it may not
  AuthUnavailable     = 3    // 503 - the plugin cannot decide (no keys, no policies yet): fail CLOSED
} AuthVerdict;



// -----------------------------------------------------------------------------
//
// AuthRequest - the request, as the broker sees it
//
typedef struct AuthRequest
{
  const char*  method;                               // "GET", "POST", ... "OPTIONS"
  const char*  path;                                 // the URL path, no query: "/ngsi-ld/v1/entities/urn:E1"
  int                 params;                        // the URL parameters, decoded, in the order sent - a name may come more than once
  const char* const*  paramKeyV;
  const char* const*  paramValueV;                   // "" for a parameter without a value
  uint64_t     op;                                   // the NGSI-LD operation - an LdOp bit (corNgsild/LdOp.h); 0: none (an admin route, /jsonldContexts, no route)
  const char*  opName;                               // its name, the broker's ldOpToString: "CreateEntity"; NULL with op 0
  const char*  route;                                // the matched route pattern ("/ngsi-ld/v1/entities/*"); NULL: no route matched (the answer will be 404/405)
  const char*  tenant;                               // authorize only - NULL: the default tenant

  const char*  (*header)(const char* name);          // a request header, by name without case; NULL: not sent

  //
  // body - the request body, as the client sent it (before any @context is applied), NULL: none. A body
  // that came as a tree (cor://) as JSON text.
  //
  const char*  (*body)(int* lenP);

  //
  // expand - authorize only: a term as the request's @context expands it ("Building" ->
  // "https://smartdatamodels.org/dataModel.Building/Building"); the term itself when the context has no
  // such term and no @vocab applies. The answer is valid for the call.
  //
  const char*  (*expand)(const char* term);

  //
  // ABI 2: expandDefault - a term as the broker's default context expands it (the core context, and
  // --defaultUserContext when there is one) - what a short name means to whoever wrote a POLICY, which is
  // not what the request's @context makes of it
  //
  const char*  (*expandDefault)(const char* term);

  //
  // ABI 2: entities - true: the broker asks entity() for EACH entity this operation reads or writes. A
  // plugin may then leave what depends on the entity to entity(); false (an operation the broker does not
  // check entity by entity), it decides here, or not at all.
  //
  bool         entities;
} AuthRequest;



// -----------------------------------------------------------------------------
//
// AuthEntity - ABI 2: one entity a request reads or writes
//
typedef struct AuthEntity
{
  const char*         id;
  const char* const*  typeV;                         // its types, EXPANDED
  int                 types;
  bool                stored;                        // true: the stored entity's types; false: as the request sends them (a create)
  const char* const*  attrV;                         // the attributes the operation touches, EXPANDED; NULL: the entity as a whole
  int                 attrs;
} AuthEntity;



// -----------------------------------------------------------------------------
//
// AuthAnswer - what goes back to the client with a verdict that is not AuthAllow. All optional (NULL).
//
typedef struct AuthAnswer
{
  const char*  title;                                // ProblemDetails title - default "Unauthorized" / "Forbidden" / "Service Unavailable"
  const char*  detail;                               // ProblemDetails detail
  const char*  wwwAuthenticate;                      // a 401's WWW-Authenticate (RFC 9110 § 11.6.1) - default: Bearer
} AuthAnswer;



// -----------------------------------------------------------------------------
//
// AuthDriver - what the plugin offers the broker
//
typedef struct AuthDriver
{
  int          abiVersion;                           // set by the BROKER before authRegister; then the plugin's
  const char*  name;                                 // "fdsc"
  const char*  version;
  CorArg*      args;                                 // its own options (--fdscVerifier, ...) - NULL: none

  //
  // init - after the options are parsed, before the first request. false: the broker does not start
  // (fail closed) - errorBuf says why.
  //
  bool         (*init)(char* errorBuf, int errorBufSize);

  //
  // authenticate - who sent it. *identityP: the plugin's own handle for the sender, handed back to
  // authorize and to release (NULL is a valid identity - "anonymous", if the plugin lets such in).
  //
  AuthVerdict  (*authenticate)(const AuthRequest* reqP, void** identityP, AuthAnswer* answerP);

  //
  // authorize - may it. NULL: authentication only, every authenticated request may.
  //
  AuthVerdict  (*authorize)(const AuthRequest* reqP, void* identity, AuthAnswer* answerP);

  //
  // release - the request is over (its response sent): the identity is the plugin's to free. NULL: nothing to free.
  //
  void         (*release)(void* identity);

  const char*  (*versionInfo)(void);                 // a line for GET /version - NULL: name + version
  void         (*close)(void);                       // the broker stops

  //
  // ABI 2: entity - may the request read or write this entity. Asked, once authorize has let the request
  // through, for each entity the operation reads or writes: the one entity of a create, a retrieve or a
  // write on it (its STORED types - the request need not carry them); each entity of a batch (a denied
  // one is that entity's error in the 207); each entity a query finds (a denied one is left out of the
  // response). NULL: authorize decides alone.
  //
  AuthVerdict  (*entity)(const AuthRequest* reqP, void* identity, const AuthEntity* entityP, AuthAnswer* answerP);

  //
  // ABI 2: identitySave / identityRestore - a subscription notifies long after the request that made it:
  // the broker keeps its maker's identity (as text, in the store, beside the subscription - never in it)
  // and decides each entity of each notification with entity() on the identity restored. identitySave:
  // malloc'd text, the broker frees it; NULL: nothing to keep (an anonymous request). identityRestore: an
  // identity of the plugin's, freed with release(); NULL: the text holds none (any more - expired): the
  // notifications go without the entities. The notification is asked about as a retrieve of each entity
  // (op LdOpRetrieveEntity, GET /ngsi-ld/v1/entities/<id>, no headers, no body). NULL slots: the broker
  // keeps nothing and filters nothing.
  //
  char*        (*identitySave)(void* identity);
  void*        (*identityRestore)(const char* saved);

  //
  // ABI 3: authorizeBeforeContext - authorize is asked at the request's admission, with authenticate, before
  // the broker resolves the request's @context (its Link header, its body's @context - URLs the CLIENT names,
  // which the broker fetches): a request that may not is refused before the broker fetches anything for it.
  // AuthRequest.expand answers NULL there (the request's @context is not known yet); everything else is as it
  // is later. false (and a plugin of ABI 2): authorize is asked after the @context, before the tenant.
  //
  bool         authorizeBeforeContext;
} AuthDriver;

typedef void (*AuthRegisterFunc)(AuthDriver* driverP);

#endif  // CORAINE_AUTHDRIVER_H_
