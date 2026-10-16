//
// FILE            auth.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <pthread.h>                                 // pthread_mutex_*
#include <stdlib.h>                                  // malloc, free
#include <time.h>                                    // time
#include <stdio.h>                                   // snprintf
#include <string.h>                                  // memset, strcasecmp, strcmp

#include "corPlugin/corPlugin.h"                     // corPluginOpen, corPluginResolve, corPluginBaseDir
#include "corLog/corLog.h"                           // COR_I, COR_W
#include "corAlloc/corAlloc.h"                       // corAlloc
#include "corJson/corJsonRender.h"                   // corJsonFastRender
#include "corJson/corJsonRenderSize.h"               // corJsonFastRenderSize
#include "corJsonld/corLdExpand.h"                   // corLdExpand
#include "corJsonld/corLdDownload.h"                 // corLdContextFromUrl
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corTree/corTreeBuilder.h"                  // corTreeChildRemove
#include "corRest/CorRestState.h"                    // corRest
#include "corRest/corRestHooks.h"                    // corRestSetAdmitHook
#include "corRest/corRestInHeader.h"                 // corRestInHeader
#include "corRest/corRestOutHeader.h"                // corRestOutHeaderAdd
#include "corRest/corRestProblem.h"                  // corRestProblem
#include "corNgsild/CorNgsild.h"                     // corNgsild, ldDefaultContextUrl
#include "corNgsild/ldSubscriptionNotify.h"          // ldNotifyFilterHook
#include "corNgsild/LdOp.h"                          // LdOp, ldOpToString
#include "corNgsild/ldIsEntityKeyword.h"             // ldIsNotAttribute

#include "db/DbDriver.h"                             // db, DB_OK
#include "db/Tenant.h"                               // Tenant
#include "troe/TroeDriver.h"                         // troe, TroeQueryFilter, TROE_OK
#include "plugin/AuthDriver.h"                       // AuthDriver, AuthRequest, AuthAnswer, AuthVerdict
#include "auth/auth.h"                               // Own interface



// -----------------------------------------------------------------------------
//
// The plugin - one at most
//
static AuthDriver  driver;
static bool        loaded = false;



// -----------------------------------------------------------------------------
//
// AUTH_ENTITY_OPS - the operations whose service routines ask entity() for every entity they read or write
// (AuthRequest.entities). An operation is added here together with its routine's call - never before: a
// plugin trusts this to leave the entity's part of a decision to entity().
//
#define AUTH_ENTITY_OPS  (LdOpCreateEntity  | LdOpRetrieveEntity | LdOpMergeEntity | LdOpReplaceEntity | LdOpDeleteEntity | \
                          LdOpAppendAttrs   | LdOpUpdateAttrs    | LdOpReplaceAttr | LdOpDeleteAttr    | \
                          LdOpBatchCreate   | LdOpBatchUpsert    | LdOpBatchUpdate | LdOpBatchMerge    | LdOpBatchDelete | \
                          LdOpQueryEntities | LdOpBatchQuery     | \
                          LdOpUpsertTemporal | LdOpAppendAttrsTemporal | LdOpDeleteAttrsTemporal | LdOpUpdateAttrInstanceTemporal | \
                          LdOpDeleteAttrInstanceTemporal | LdOpDeleteTemporal | LdOpRetrieveTemporal | LdOpQueryTemporal | \
                          LdOpRetrieveEntityTimepoint | LdOpQueryEntityTimepoint)   // getEntity / getEntities run them



// -----------------------------------------------------------------------------
//
// AuthState - a request's, in its arena (corNgsild.authP)
//
typedef struct AuthState
{
  void*  identity;                                   // the plugin's handle for the sender
  bool   trusted;                                    // the broker's own request (authTrustNext): never asked
  bool   released;
  char*  body;                                       // the body as sent (bodyKeep)
  int    bodyLen;
  char*  path;                                       // the URL path as sent (pathKeep)
  bool   authorized;                                 // authorize asked already, at admission (authorizeBeforeContext)
  char*  checkedId;                                  // the entity authEntityStoredCheck asked about whole ...
  bool   checkedPartial;                             // ... and whether only some of its attributes may be read
} AuthState;



// -----------------------------------------------------------------------------
//
// trustNext - authTrustNext's mark, consumed by the next admission on this thread. A thread-local and not
// request state on purpose: the request it marks does not exist yet when it is set.
//
static __thread bool trustNext = false;



// -----------------------------------------------------------------------------
//
// headerSizeCheck - a large Authorization header, said once: received with every request, its size is a cost
// of each, and the client's choice (doc/performance.md, "The token's size"). Looked at on a thread's first
// request and every 1024th after it, until said.
//
static bool              headerSizeSaid = false;
static __thread unsigned headerSizeSeen = 0;

static void headerSizeCheck(void)
{
  if ((__atomic_load_n(&headerSizeSaid, __ATOMIC_RELAXED) == true) || ((headerSizeSeen++ & 1023) != 0))
    return;

  const char* value = corRestInHeader("Authorization");
  size_t      len   = (value != NULL) ? strlen(value) : 0;

  if ((len > 512) && (__atomic_exchange_n(&headerSizeSaid, true, __ATOMIC_RELAXED) == false))
    COR_W("requests carry a %zu-byte Authorization header - received with every request, its size is a cost of each (doc/performance.md, \"The token's size\")", len);
}



// -----------------------------------------------------------------------------
//
// bodyKeep - the body as the client sent it, kept at admission: the parse that follows works in place, and
// applies the @context - by authorize, neither the text nor the tree is what was sent. A tree (cor://) is
// rendered.
//
static void bodyKeep(AuthState* stateP)
{
  if ((corRest.in.payload != NULL) && (corRest.in.payloadSize > 0))
  {
    stateP->body = (char*) corAlloc(&corRest.kalloc, corRest.in.payloadSize + 1);

    if (stateP->body != NULL)
    {
      memcpy(stateP->body, corRest.in.payload, corRest.in.payloadSize);
      stateP->body[corRest.in.payloadSize] = 0;
      stateP->bodyLen = corRest.in.payloadSize;
    }
  }
  else if (corRest.in.requestTree != NULL)
  {
    stateP->body = (char*) corAlloc(&corRest.kalloc, corJsonFastRenderSize(corRest.in.requestTree) + 1);

    if (stateP->body != NULL)
    {
      corJsonFastRender(corRest.in.requestTree, stateP->body);
      stateP->bodyLen = strlen(stateP->body);
    }
  }
}



// -----------------------------------------------------------------------------
//
// pathKeep - the URL path, whole: the route lookup has cut it where a wildcard's value ends (corRest
// NUL-terminates the values in place) - put back together from the route and the values
//
static void pathKeep(AuthState* stateP)
{
  const char* path = (corRest.in.urlPath != NULL) ? corRest.in.urlPath : "/";

  int wildcards = (corRest.serviceP != NULL) ? corRest.serviceP->wildcards : 0;

  if (wildcards == 0)
  {
    stateP->path = (char*) path;
    return;
  }

  int len = strlen(corRest.serviceP->url) + 1;

  for (int i = 0; i < wildcards; i++)
    len += (corRest.in.wildcard[i] != NULL) ? strlen(corRest.in.wildcard[i]) : 0;

  char* buf = (char*) corAlloc(&corRest.kalloc, len);

  if (buf == NULL)
  {
    stateP->path = (char*) path;
    return;
  }

  const char* p  = corRest.serviceP->url;
  char*       o  = buf;
  int         wx = 0;

  while (*p != 0)
  {
    if (*p == '*')
    {
      if (p[1] == '*')                               // ** - a greedy wildcard, one value
        ++p;

      const char* v = (wx < wildcards) ? corRest.in.wildcard[wx] : NULL;

      ++wx;
      if (v != NULL)
      {
        int vLen = strlen(v);
        memcpy(o, v, vLen);
        o += vLen;
      }
      ++p;
      continue;
    }

    *o++ = *p++;
  }

  *o = 0;
  stateP->path = buf;
}



// -----------------------------------------------------------------------------
//
// bodyText - AuthRequest.body
//
static const char* bodyText(int* lenP)
{
  AuthState* stateP = (AuthState*) corNgsild.authP;

  if (lenP != NULL)
    *lenP = ((stateP != NULL) && (stateP->body != NULL)) ? stateP->bodyLen : 0;

  return (stateP != NULL) ? stateP->body : NULL;
}



// -----------------------------------------------------------------------------
//
// termExpand - AuthRequest.expand: a term with the request's @context
//
static const char* termExpandNotYet(const char* term)
{
  (void) term;
  return NULL;                                       // at admission: the request's @context is not resolved yet
}

static const char* termExpand(const char* term)
{
  if (term == NULL)
    return NULL;

  char* iri = corLdExpand(corNgsild.contextP, term, &corRest.kalloc, NULL, NULL);

  return (iri != NULL) ? iri : term;
}



// -----------------------------------------------------------------------------
//
// termExpandDefault - AuthRequest.expandDefault: a term with the broker's default context (core, and
// --defaultUserContext)
//
static const char* termExpandDefault(const char* term)
{
  if (term == NULL)
    return NULL;

  CorLdContext* contextP = (ldDefaultContextUrl != NULL) ? corLdContextFromUrl(ldDefaultContextUrl, &corRest.kalloc) : NULL;
  char*         iri      = corLdExpand(contextP, term, &corRest.kalloc, NULL, NULL);

  return (iri != NULL) ? iri : term;
}



// -----------------------------------------------------------------------------
//
// requestFill - the AuthRequest of the bound request
//
static void requestFill(AuthRequest* reqP, bool withTenant, bool atAdmission)
{
  uint64_t op = (corRest.serviceP != NULL) ? corRest.serviceP->ldOp : 0;

  memset(reqP, 0, sizeof(AuthRequest));

  reqP->method = (corRest.in.verbString != NULL) ? corRest.in.verbString : "?";
  reqP->path   = ((corNgsild.authP != NULL) && (((AuthState*) corNgsild.authP)->path != NULL)) ? ((AuthState*) corNgsild.authP)->path : ((corRest.in.urlPath != NULL) ? corRest.in.urlPath : "/");
  if (corRest.in.uriParamCount > 0)
  {
    const char** keyV   = (const char**) corAlloc(&corRest.kalloc, corRest.in.uriParamCount * sizeof(char*));
    const char** valueV = (const char**) corAlloc(&corRest.kalloc, corRest.in.uriParamCount * sizeof(char*));

    if ((keyV != NULL) && (valueV != NULL))
    {
      for (int i = 0; i < corRest.in.uriParamCount; i++)
      {
        keyV[i]   = corRest.in.uriParamV[i].key;
        valueV[i] = (corRest.in.uriParamV[i].value != NULL) ? corRest.in.uriParamV[i].value : "";
      }

      reqP->params      = corRest.in.uriParamCount;
      reqP->paramKeyV   = keyV;
      reqP->paramValueV = valueV;
    }
  }

  reqP->op     = op;
  reqP->opName = (op != 0) ? ldOpToString((LdOp) op) : NULL;
  reqP->route  = (corRest.serviceP != NULL) ? corRest.serviceP->url : NULL;
  reqP->header = corRestInHeader;
  reqP->body   = bodyText;

  if (withTenant == true)
  {
    const char* tenant = corRestInHeader("NGSILD-Tenant");

    reqP->tenant        = ((tenant != NULL) && (tenant[0] != 0)) ? tenant : NULL;
    reqP->expand        = (atAdmission == true) ? termExpandNotYet : termExpand;
    reqP->expandDefault = termExpandDefault;
    reqP->entities      = (driver.abiVersion >= 2) && (driver.entity != NULL) && ((op & AUTH_ENTITY_OPS) != 0);
  }
}



// -----------------------------------------------------------------------------
//
// answer - the error response of a verdict that is not AuthAllow
//
static void answer(AuthVerdict verdict, AuthAnswer* answerP)
{
  int         status = 403;
  const char* title  = "Forbidden";

  if (verdict == AuthUnauthenticated)
  {
    status = 401;
    title  = "Unauthorized";
    //
    // Copied: the plugin's text may be a buffer of its thread, and the header is written after the call
    //
    corRestOutHeaderAdd("WWW-Authenticate", (answerP->wwwAuthenticate != NULL) ? corAllocStrdup(&corRest.kalloc, answerP->wwwAuthenticate) : "Bearer");
  }
  else if (verdict == AuthUnavailable)
  {
    status = 503;
    title  = "Service Unavailable";
  }

  if (answerP->title != NULL)
    title = answerP->title;

  //
  // RFC 9457 § 4.2.1 "about:blank": NGSI-LD has no error type for either (TS 104 175 § 5.5.2 lists none
  // bound to 401, and 403 only for OperationNotSupported and TooManyResults)
  //
  corRestProblem(status, "about:blank", title, "%s", (answerP->detail != NULL) ? answerP->detail : title);
}



// -----------------------------------------------------------------------------
//
// admit - corRest's admit hook: who sent it
//
static bool admit(void)
{
  AuthState* stateP = (AuthState*) corAlloc(&corRest.kalloc, sizeof(AuthState));

  if (stateP == NULL)
  {
    trustNext = false;
    corRestProblem(503, "about:blank", "Service Unavailable", "out of memory");
    return false;
  }

  memset(stateP, 0, sizeof(AuthState));
  corNgsild.authP = stateP;

  if (trustNext == true)
  {
    trustNext       = false;
    stateP->trusted = true;
    return true;
  }

  //
  // The second listener's - the policy API (--policyApiPort): no authentication, as the PAP's; whoever
  // reaches its port may
  //
  if (corRest.listener != 0)
  {
    stateP->trusted = true;
    return true;
  }

  headerSizeCheck();
  bodyKeep(stateP);
  pathKeep(stateP);

  AuthRequest  req;
  AuthAnswer   ans;
  void*        identity = NULL;

  requestFill(&req, false, true);
  memset(&ans, 0, sizeof(ans));

  AuthVerdict verdict = driver.authenticate(&req, &identity, &ans);

  stateP->identity = identity;

  if (verdict != AuthAllow)
  {
    answer(verdict, &ans);
    return false;
  }

  //
  // May it - here already, for a plugin that does not need the request's @context: before the broker
  // resolves it, and fetches what the client named for it
  //
  if ((driver.abiVersion < 3) || (driver.authorizeBeforeContext == false) || (driver.authorize == NULL))
    return true;

  requestFill(&req, true, true);
  memset(&ans, 0, sizeof(ans));

  verdict = driver.authorize(&req, stateP->identity, &ans);

  if (verdict != AuthAllow)
  {
    answer(verdict, &ans);
    return false;
  }

  stateP->authorized = true;
  return true;
}



// =============================================================================
//
// A subscription's owner - who made it, for its notifications (ABI 2: identitySave / identityRestore)
//
// The plugin's text, kept in the store's document collection "subscriptionOwners" - never inside the
// subscription, so nothing that renders a subscription can show it. Restored identities are cached here,
// shared by the notifying threads (counted, freed when the last lets go), and read again after OWNER_TTL
// seconds - another instance (HA) may have stored a new owner meanwhile.
//
// =============================================================================

#define OWNER_COLLECTION  "subscriptionOwners"
#define OWNER_TTL         30
#define OWNER_BUCKETS     256

typedef struct Owner
{
  char*          key;                                // <tenant>|<subId>
  void*          identity;                           // NULL: none - the notifications go without their entities
  int            refs;                               // the cache's own, and each notification's using it
  time_t         loadedAt;
  bool           dropped;                            // out of the cache - freed at its last release
  struct Owner*  next;
} Owner;

static pthread_mutex_t  ownerMutex = PTHREAD_MUTEX_INITIALIZER;
static Owner*           ownerV[OWNER_BUCKETS];



// -----------------------------------------------------------------------------
//
// ownerKey - <tenant>|<subId>, malloc'd
//
static char* ownerKey(Tenant* tenantP, const char* subId)
{
  const char* tenant = ((tenantP != NULL) && (tenantP->name[0] != 0)) ? tenantP->name : "";
  size_t      len    = strlen(tenant) + 1 + strlen(subId) + 1;
  char*       key    = (char*) malloc(len);

  if (key != NULL)
    snprintf(key, len, "%s|%s", tenant, subId);

  return key;
}

static unsigned int ownerBucket(const char* key)
{
  unsigned int h = 5381;

  for (const char* c = key; *c != 0; c++)
    h = h * 33 + (unsigned char) *c;

  return h % OWNER_BUCKETS;
}



// -----------------------------------------------------------------------------
//
// ownerRelease - a reference let go; the last of a dropped owner frees it. Under ownerMutex.
//
static void ownerRelease(Owner* oP)
{
  if (--oP->refs > 0)
    return;

  if ((oP->identity != NULL) && (driver.release != NULL))
    driver.release(oP->identity);

  free(oP->key);
  free(oP);
}



// -----------------------------------------------------------------------------
//
// ownerDrop - the cache's entry for 'key' out of the cache. Under ownerMutex.
//
static void ownerDrop(const char* key)
{
  Owner** prevPP = &ownerV[ownerBucket(key)];

  for (Owner* oP = *prevPP; oP != NULL; prevPP = &oP->next, oP = oP->next)
  {
    if (strcmp(oP->key, key) == 0)
    {
      *prevPP     = oP->next;
      oP->dropped = true;
      ownerRelease(oP);                              // the cache's reference
      return;
    }
  }
}



// -----------------------------------------------------------------------------
//
// ownerGet - the owner of a subscription, referenced (ownerPut when done) - from the cache, or read from the
// store and restored. NULL only when out of memory.
//
static Owner* ownerGet(Tenant* tenantP, const char* subId)
{
  char* key = ownerKey(tenantP, subId);

  if (key == NULL)
    return NULL;

  time_t now = time(NULL);

  pthread_mutex_lock(&ownerMutex);

  for (Owner* oP = ownerV[ownerBucket(key)]; oP != NULL; oP = oP->next)
  {
    if ((strcmp(oP->key, key) == 0) && (now - oP->loadedAt < OWNER_TTL))
    {
      oP->refs += 1;
      pthread_mutex_unlock(&ownerMutex);
      free(key);
      return oP;
    }
  }

  pthread_mutex_unlock(&ownerMutex);

  //
  // Read from the store - outside the lock: it may wait for the database
  //
  void*    identity = NULL;
  CorNode* docP     = NULL;

  if ((db.docRetrieve != NULL) && (db.docRetrieve(tenantP, OWNER_COLLECTION, subId, &docP) == DB_OK) && (docP != NULL))
  {
    CorNode* textP = corTreeLookup(docP, "owner");

    if ((textP != NULL) && (textP->type == CorString))
      identity = driver.identityRestore(textP->value.s);
  }

  Owner* oP = (Owner*) calloc(1, sizeof(Owner));

  if (oP == NULL)
  {
    if ((identity != NULL) && (driver.release != NULL))
      driver.release(identity);
    free(key);
    return NULL;
  }

  oP->key      = key;
  oP->identity = identity;
  oP->refs     = 2;                                  // the cache's and the caller's
  oP->loadedAt = now;

  pthread_mutex_lock(&ownerMutex);
  ownerDrop(key);                                    // a stale one, or one another thread loaded meanwhile
  unsigned int b = ownerBucket(key);
  oP->next  = ownerV[b];
  ownerV[b] = oP;
  pthread_mutex_unlock(&ownerMutex);

  return oP;
}

static void ownerPut(Owner* oP)
{
  pthread_mutex_lock(&ownerMutex);
  ownerRelease(oP);
  pthread_mutex_unlock(&ownerMutex);
}



// -----------------------------------------------------------------------------
//
// nullHeader, nullBody - a notification's AuthRequest has neither
//
static const char* nullHeader(const char* name)  { (void) name; return NULL; }
static const char* nullBody(int* lenP)           { if (lenP != NULL) *lenP = 0; return NULL; }



// -----------------------------------------------------------------------------
//
// notifyEntityAllowed - may the owner read this entity: asked as a retrieve of it
//
static bool notifyEntityAllowed(Tenant* tenantP, void* identity, CorNode* entityP, CorAlloc* kaP)
{
  CorNode* idP   = (entityP->type == CorObject) ? corTreeLookup(entityP, "id") : NULL;
  CorNode* typeP = (entityP->type == CorObject) ? corTreeLookup(entityP, "type") : NULL;

  if ((idP == NULL) || (idP->type != CorString))
    return false;

  const char*  one[1];
  const char** typeV = one;
  int          types = 0;

  if ((typeP != NULL) && (typeP->type == CorString))
  {
    one[0] = typeP->value.s;
    types  = 1;
  }
  else if ((typeP != NULL) && (typeP->type == CorArray))
  {
    int n = 0;
    for (CorNode* tP = typeP->value.head; tP != NULL; tP = tP->next)
      ++n;

    typeV = (const char**) corAlloc(kaP, (n + 1) * sizeof(char*));
    if (typeV == NULL)
      return false;

    for (CorNode* tP = typeP->value.head; tP != NULL; tP = tP->next)
    {
      if (tP->type == CorString)
        typeV[types++] = tP->value.s;
    }
  }

  size_t pathLen = strlen("/ngsi-ld/v1/entities/") + strlen(idP->value.s) + 1;
  char*  path    = (char*) corAlloc(kaP, pathLen);

  if (path == NULL)
    return false;

  snprintf(path, pathLen, "/ngsi-ld/v1/entities/%s", idP->value.s);

  AuthRequest req;
  AuthAnswer  ans;
  AuthEntity  entity;

  memset(&req, 0, sizeof(req));
  memset(&ans, 0, sizeof(ans));

  req.method        = "GET";
  req.path          = path;
  req.op            = LdOpRetrieveEntity;
  req.opName        = ldOpToString(LdOpRetrieveEntity);
  req.route         = "/ngsi-ld/v1/entities/*";
  req.tenant        = ((tenantP != NULL) && (tenantP->name[0] != 0)) ? tenantP->name : NULL;
  req.header        = nullHeader;
  req.body          = nullBody;
  req.expand        = termExpandDefault;             // no request, no @context of its own
  req.expandDefault = termExpandDefault;
  req.entities      = true;

  entity.id     = idP->value.s;
  entity.typeV  = typeV;
  entity.types  = types;
  entity.stored = true;
  entity.attrV  = NULL;
  entity.attrs  = 0;

  if (driver.entity(&req, identity, &entity, &ans) != AuthAllow)
    return false;

  if ((driver.abiVersion < 4) || (ans.partial == false))
    return true;

  //
  // Only some of its attributes: each one asked about - the ones no policy allows left out of the notification
  //
  CorNode* aP = entityP->value.head;

  while (aP != NULL)
  {
    CorNode* nextP = aP->next;

    if (ldIsNotAttribute(aP) == false)
    {
      const char* one[1] = { aP->name };

      memset(&ans, 0, sizeof(ans));
      entity.attrV = one;
      entity.attrs = 1;

      if (driver.entity(&req, identity, &entity, &ans) != AuthAllow)
        corTreeChildRemove(entityP, aP);
    }

    aP = nextP;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// notifyFilter - corNgsild's ldNotifyFilterHook: the entities the subscription's owner may not read go;
// an owner not known (none kept, none any more) - all of them go: fail closed. A plugin with notify (ABI 5)
// is asked first, once: not allowed - all of them go
//
static void notifyFilter(void* tenantP, const char* subId, CorNode* dataArrayP, CorAlloc* kaP)
{
  if ((dataArrayP == NULL) || (dataArrayP->type != CorArray) || (subId == NULL))
    return;

  Owner* oP = ownerGet((Tenant*) tenantP, subId);

  if ((driver.abiVersion >= 5) && (driver.notify != NULL))
  {
    Tenant*     tP     = (Tenant*) tenantP;
    const char* tenant = ((tP != NULL) && (tP->name[0] != 0)) ? tP->name : NULL;

    if ((oP == NULL) || (oP->identity == NULL) || (driver.notify(oP->identity, tenant) != AuthAllow))
    {
      dataArrayP->value.head = NULL;                 // nothing left: the notification is not sent
      dataArrayP->value.tail = NULL;
    }

    if ((driver.entity == NULL) || (dataArrayP->value.head == NULL))
    {
      if (oP != NULL)
        ownerPut(oP);
      return;
    }
  }

  CorNode* eP = dataArrayP->value.head;

  while (eP != NULL)
  {
    CorNode* nextP = eP->next;

    if ((oP == NULL) || (oP->identity == NULL) || (notifyEntityAllowed((Tenant*) tenantP, oP->identity, eP, kaP) == false))
      corTreeChildRemove(dataArrayP, eP);

    eP = nextP;
  }

  if (oP != NULL)
    ownerPut(oP);
}



// -----------------------------------------------------------------------------
//
// authSubscriptionOwnerKeep -
//
void authSubscriptionOwnerKeep(const char* subId)
{
  if ((loaded == false) || (driver.abiVersion < 2) || (driver.identitySave == NULL) || (subId == NULL) || (db.docCreate == NULL))
    return;

  AuthState* stateP = (AuthState*) corNgsild.authP;

  if ((stateP == NULL) || (stateP->trusted == true))   // the broker's own request: the owner stays who it was
    return;

  Tenant* tenantP = (Tenant*) corNgsild.tenantP;
  char*   text    = driver.identitySave(stateP->identity);

  if (text == NULL)                                  // no one: an owner kept before goes
  {
    authSubscriptionOwnerForget(subId);
    return;
  }

  CorNode* docP = corTreeObject(corRest.kallocP, NULL);

  corTreeChildAdd(docP, corTreeString(corRest.kallocP, "id",    corAllocStrdup(&corRest.kalloc, subId)));
  corTreeChildAdd(docP, corTreeString(corRest.kallocP, "owner", corAllocStrdup(&corRest.kalloc, text)));
  free(text);

  if (db.docReplace(tenantP, OWNER_COLLECTION, subId, docP) == DB_NOT_FOUND)
    db.docCreate(tenantP, OWNER_COLLECTION, subId, docP);

  char* key = ownerKey(tenantP, subId);

  if (key != NULL)
  {
    pthread_mutex_lock(&ownerMutex);
    ownerDrop(key);                                  // the next notification reads the new one
    pthread_mutex_unlock(&ownerMutex);
    free(key);
  }
}



// -----------------------------------------------------------------------------
//
// authSubscriptionOwnerForget -
//
void authSubscriptionOwnerForget(const char* subId)
{
  if ((loaded == false) || (subId == NULL) || (db.docDelete == NULL))
    return;

  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

  db.docDelete(tenantP, OWNER_COLLECTION, subId);

  char* key = ownerKey(tenantP, subId);

  if (key != NULL)
  {
    pthread_mutex_lock(&ownerMutex);
    ownerDrop(key);
    pthread_mutex_unlock(&ownerMutex);
    free(key);
  }
}



// -----------------------------------------------------------------------------
//
// authLoad -
//
int authLoad(const char* name, char* errorBuf, int errorBufSize)
{
  char path[512];
  char openErr[512];

  corPluginResolve(corPluginBaseDir(), "auth", NULL, name, path, sizeof(path));

  AuthRegisterFunc registerFunc = (AuthRegisterFunc) corPluginOpen(path, "authRegister", openErr, sizeof(openErr));

  if (registerFunc == NULL)
  {
    snprintf(errorBuf, errorBufSize, "auth plugin '%s' (%s): %s", name, path, openErr);
    return -1;
  }

  memset(&driver, 0, sizeof(driver));
  driver.abiVersion = AUTH_ABI_VERSION;
  registerFunc(&driver);

  if (driver.authenticate == NULL)
  {
    snprintf(errorBuf, errorBufSize, "auth plugin '%s' (%s): no authenticate function", name, path);
    return -1;
  }

  loaded = true;
  COR_I("auth plugin '%s' loaded (%s, ABI %d)", (driver.name != NULL) ? driver.name : name, path, driver.abiVersion);
  return 0;
}



// -----------------------------------------------------------------------------
//
// authArgs -
//
CorArg* authArgs(void)
{
  return (loaded == true) ? driver.args : NULL;
}



// -----------------------------------------------------------------------------
//
// authPluginName -
//
const char* authPluginName(void)
{
  return (loaded == true) ? driver.name : NULL;
}



// -----------------------------------------------------------------------------
//
// authInit -
//
bool authInit(void)
{
  if (loaded == false)
    return true;

  char errorBuf[512] = "";

  if ((driver.init != NULL) && (driver.init(errorBuf, sizeof(errorBuf)) == false))
  {
    COR_E("auth plugin '%s': %s", driver.name, (errorBuf[0] != 0) ? errorBuf : "init failed");
    return false;
  }

  corRestSetAdmitHook(admit);

  //
  // The notifications - decided for their subscription's owner, when the plugin can (after its init: a
  // plugin may take its slots back there)
  //
  bool decides = (driver.entity != NULL) || ((driver.abiVersion >= 5) && (driver.notify != NULL));

  if ((driver.abiVersion >= 2) && (decides == true) && (driver.identitySave != NULL) && (driver.identityRestore != NULL))
    ldNotifyFilterHook = notifyFilter;

  return true;
}



// -----------------------------------------------------------------------------
//
// authAuthorize -
//
bool authAuthorize(void)
{
  if ((loaded == false) || (driver.authorize == NULL))
    return true;

  AuthState* stateP = (AuthState*) corNgsild.authP;

  //
  // No state: a request that was never admitted - an in-process forward (corRest does not ask for one,
  // its request was let in when it arrived). The broker's own: trusted.
  //
  if ((stateP == NULL) || (stateP->trusted == true) || (stateP->authorized == true))
    return true;

  AuthRequest  req;
  AuthAnswer   ans;

  requestFill(&req, true, false);
  memset(&ans, 0, sizeof(ans));

  AuthVerdict verdict = driver.authorize(&req, stateP->identity, &ans);

  if (verdict == AuthAllow)
    return true;

  answer(verdict, &ans);
  return false;
}



// -----------------------------------------------------------------------------
//
// authEntityWanted -
//
bool authEntityWanted(void)
{
  if ((loaded == false) || (driver.abiVersion < 2) || (driver.entity == NULL))
    return false;

  AuthState* stateP = (AuthState*) corNgsild.authP;

  return (stateP != NULL) && (stateP->trusted == false);
}



// -----------------------------------------------------------------------------
//
// authEntityAllowed -
//
static bool entityAsk(const char* id, const char* const* typeV, int types, bool stored, const char* const* attrV, int attrs, bool* partialP, const char** detailP)
{
  AuthState*   stateP = (AuthState*) corNgsild.authP;
  AuthRequest  req;
  AuthAnswer   ans;
  AuthEntity   entity;

  requestFill(&req, true, false);
  memset(&ans, 0, sizeof(ans));

  entity.id     = id;
  entity.typeV  = typeV;
  entity.types  = types;
  entity.stored = stored;
  entity.attrV  = attrV;
  entity.attrs  = attrs;

  AuthVerdict verdict = driver.entity(&req, stateP->identity, &entity, &ans);

  if (verdict == AuthAllow)
  {
    if (partialP != NULL)
      *partialP = (driver.abiVersion >= 4) && (ans.partial == true);
    return true;
  }

  if (detailP != NULL)
    *detailP = (ans.detail != NULL) ? corAllocStrdup(&corRest.kalloc, ans.detail) : "not allowed";

  return false;
}



bool authEntityAllowed(const char* id, const char* const* typeV, int types, bool stored, const char* const* attrV, int attrs, const char** detailP)
{
  if (authEntityWanted() == false)
    return true;

  return entityAsk(id, typeV, types, stored, attrV, attrs, NULL, detailP);
}



// -----------------------------------------------------------------------------
//
// authEntityTreeAllowed - authEntityAllowed for an entity whose types are a tree: "T" or ["T1", "T2"],
// expanded
//
static const char** typesOf(CorNode* typeP, int* typesP)
{
  *typesP = 0;

  if (typeP == NULL)
    return NULL;

  int n = (typeP->type == CorString) ? 1 : 0;

  if (typeP->type == CorArray)
  {
    for (CorNode* tP = typeP->value.head; tP != NULL; tP = tP->next)
      ++n;
  }

  const char** typeV = (const char**) corAlloc(&corRest.kalloc, (n + 1) * sizeof(char*));

  if (typeV == NULL)
    return NULL;

  if (typeP->type == CorString)
    typeV[(*typesP)++] = typeP->value.s;
  else if (typeP->type == CorArray)
  {
    for (CorNode* tP = typeP->value.head; tP != NULL; tP = tP->next)
    {
      if (tP->type == CorString)
        typeV[(*typesP)++] = tP->value.s;
    }
  }

  return typeV;
}



//
// attrsOfBody - the attributes an entity's body (a create, a batch entry, a PATCH / POST /attrs, a merge)
// writes: its members that are attributes, expanded with the request's @context
//
static const char** attrsOfBody(CorNode* bodyP, int* attrsP)
{
  *attrsP = 0;

  if ((bodyP == NULL) || (bodyP->type != CorObject))
    return NULL;

  int n = 0;
  for (CorNode* aP = bodyP->value.head; aP != NULL; aP = aP->next)
  {
    if (ldIsNotAttribute(aP) == false)
      ++n;
  }

  if (n == 0)
    return NULL;

  const char** attrV = (const char**) corAlloc(&corRest.kalloc, n * sizeof(char*));

  if (attrV == NULL)
    return NULL;

  for (CorNode* aP = bodyP->value.head; aP != NULL; aP = aP->next)
  {
    if (ldIsNotAttribute(aP) == false)
      attrV[(*attrsP)++] = termExpand(aP->name);
  }

  return attrV;
}



//
// requestAttrs - the attributes the bound request writes (or, GET .../attrs/{attr}, reads): the path's
// attribute, else the body's for the routes whose body is attributes. NULL: the entity whole
//
static const char** requestAttrs(int* attrsP)
{
  *attrsP = 0;

  const char* route = (corRest.serviceP != NULL) ? corRest.serviceP->url : NULL;

  if (route == NULL)
    return NULL;

  if ((strstr(route, "/attrs/*") != NULL) && (corRest.in.wildcard[1] != NULL))
  {
    const char** attrV = (const char**) corAlloc(&corRest.kalloc, sizeof(char*));

    if (attrV == NULL)
      return NULL;

    attrV[0] = termExpand(corRest.in.wildcard[1]);
    *attrsP  = 1;
    return attrV;
  }

  size_t len       = strlen(route);
  bool   attrsBody = (len >= 6) && (strcmp(&route[len - 6], "/attrs") == 0) && (corRest.in.verb != CorVerbGet);
  bool   merge     = (corRest.in.verb == CorVerbPatch) && (strcmp(route, "/ngsi-ld/v1/entities/*") == 0);

  if ((attrsBody == true) || (merge == true))
    return attrsOfBody(corRest.in.requestTree, attrsP);

  return NULL;
}



bool authEntityTreeAllowed(const char* id, CorNode* typeP, bool stored, const char** detailP)
{
  if (authEntityWanted() == false)
    return true;

  int          types = 0;
  const char** typeV = typesOf(typeP, &types);

  return entityAsk(id, typeV, types, stored, NULL, 0, NULL, detailP);
}



// -----------------------------------------------------------------------------
//
// storedTypes - an entity's stored types: its current state's, else its history's (the temporal store -
// an entity that has history and no current state: a deleted one, one made through the temporal API); NULL:
// not stored here
//
static CorNode* storedTypes(const char* id)
{
  CorNode* entityP = NULL;

  if ((corNgsild.tenantP != NULL) && (db.entityTypes != NULL))   // the types alone - no copy of the attributes
  {
    CorNode* typeP = NULL;

    if ((db.entityTypes((Tenant*) corNgsild.tenantP, id, &typeP) == DB_OK) && (typeP != NULL))
      return typeP;
  }
  else if ((corNgsild.tenantP != NULL) && (db.entityRetrieve != NULL) &&
      (db.entityRetrieve((Tenant*) corNgsild.tenantP, id, &entityP) == DB_OK) && (entityP != NULL))
    return corTreeLookup(entityP, "type");

  if ((corNgsild.tenantP != NULL) && (troe.entityTemporalRetrieve != NULL))
  {
    TroeQueryFilter filter;
    TroeRangeInfo   range;
    CorNode*        resultP = NULL;

    memset(&filter, 0, sizeof(filter));
    memset(&range,  0, sizeof(range));
    filter.lastN = 1;                                // the types, not the history

    if ((troe.entityTemporalRetrieve((Tenant*) corNgsild.tenantP, id, &filter, &resultP, &range) == TROE_OK) && (resultP != NULL))
      return corTreeLookup(resultP, "type");
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// authEntityAttrsFilter -
//
bool authEntityAttrsFilter(CorNode* entityP)
{
  if ((authEntityWanted() == false) || (entityP == NULL) || (entityP->type != CorObject))
    return true;

  CorNode* idP = corTreeLookup(entityP, "id");

  if ((idP == NULL) || (idP->type != CorString))
    return false;

  AuthState* stateP  = (AuthState*) corNgsild.authP;
  bool       checked = ((stateP->checkedId != NULL) && (strcmp(stateP->checkedId, idP->value.s) == 0));   // authEntityStoredCheck asked already

  if ((checked == true) && (stateP->checkedPartial == false))
    return true;

  //
  // Its types as the entity has them - or, when a projection (pick) took them out, as stored
  //
  CorNode* typeP = corTreeLookup(entityP, "type");

  if (typeP == NULL)
    typeP = storedTypes(idP->value.s);

  int          types   = 0;
  const char** typeV   = typesOf(typeP, &types);
  bool         partial = checked;

  if ((checked == false) && (entityAsk(idP->value.s, typeV, types, true, NULL, 0, &partial, NULL) == false))
    return false;

  if (partial == false)
    return true;

  //
  // Only some of its attributes: each one asked about - the ones no policy allows left out
  //
  CorNode* aP = entityP->value.head;

  while (aP != NULL)
  {
    CorNode* nextP = aP->next;

    if (ldIsNotAttribute(aP) == false)
    {
      const char* one[1] = { aP->name };

      if (entityAsk(idP->value.s, typeV, types, true, one, 1, NULL, NULL) == false)
        corTreeChildRemove(entityP, aP);
    }

    aP = nextP;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// authEntityStoredCheck -
//
bool authEntityStoredCheck(const char* id)
{
  if (authEntityWanted() == false)
    return true;

  //
  // The STORED entity's types - looked up here, before the routine forwards or writes anything. An entity
  // that is not here locally has no types: what depends on its type does not hold for it
  //
  CorNode*     typeP  = storedTypes(id);
  const char*  detail = NULL;
  int          types  = 0;
  const char** typeV  = typesOf(typeP, &types);
  int          attrs  = 0;
  const char** attrV  = requestAttrs(&attrs);       // the attributes it writes (or reads, by name): each must be allowed

  bool         partial = false;

  if (entityAsk(id, typeV, types, true, attrV, attrs, &partial, &detail) == false)
  {
    authEntityDeny(detail);
    return false;
  }

  if (attrs == 0)                                    // asked about whole: authEntityAttrsFilter needs not ask again
  {
    AuthState* stateP = (AuthState*) corNgsild.authP;

    stateP->checkedId      = corAllocStrdup(&corRest.kalloc, id);
    stateP->checkedPartial = partial;
  }

  //
  // A write that gives the entity types (a replace, a merge): those are asked about too - changing an
  // entity's type, or adding one, needs the right to the type it gets. Only where the body is an entity - an attribute's body has a "type" of its own ("Property")
  //
  uint64_t op       = (corRest.serviceP != NULL) ? corRest.serviceP->ldOp : 0;
  bool     isEntity = ((op & (LdOpReplaceEntity | LdOpMergeEntity)) != 0);   // (a temporal write's body is attributes - their instances)
  CorNode* bodyTypeP = ((isEntity == true) && (corRest.in.requestTree != NULL) && (corRest.in.requestTree->type == CorObject)) ? corTreeLookup(corRest.in.requestTree, "type") : NULL;

  if ((bodyTypeP != NULL) && (authEntityTreeAllowed(id, bodyTypeP, false, &detail) == false))
  {
    authEntityDeny(detail);
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// authEntityBatchAllowed -
//
bool authEntityBatchAllowed(const char* id, CorNode* entryP, bool lookup, const char** detailP)
{
  if (authEntityWanted() == false)
    return true;

  CorNode*     bodyTypeP = ((entryP != NULL) && (entryP->type == CorObject)) ? corTreeLookup(entryP, "type") : NULL;
  int          attrs     = 0;
  const char** attrV     = attrsOfBody(entryP, &attrs);   // the attributes the entry writes: each must be allowed (none - a delete: the entity whole)
  CorNode*     storedP   = (lookup == true) ? storedTypes(id) : NULL;
  int          types     = 0;
  const char** typeV;

  if (storedP != NULL)
  {
    typeV = typesOf(storedP, &types);

    if (entityAsk(id, typeV, types, true, attrV, attrs, NULL, detailP) == false)
      return false;

    if (bodyTypeP == NULL)                           // the types the body gives it, too: changing or adding a type needs the right to it
      return true;
  }

  typeV = typesOf(bodyTypeP, &types);
  return entityAsk(id, typeV, types, false, attrV, attrs, NULL, detailP);
}



// -----------------------------------------------------------------------------
//
// authEntityArrayFilter -
//
bool authEntityArrayFilter(CorNode* arrayP)
{
  if ((authEntityWanted() == false) || (arrayP == NULL) || (arrayP->type != CorArray))
    return true;

  //
  // join=inline nests the linked entities inside the ones that link to them - none of them is asked
  // about. Refused rather than answered with what may not be read.
  //
  if ((corNgsild.join != NULL) && (strcmp(corNgsild.join, "inline") == 0))
  {
    authEntityDeny("join=inline is not decided entity by entity - use join=flat");
    return false;
  }

  CorNode* eP = arrayP->value.head;

  while (eP != NULL)
  {
    CorNode* nextP = eP->next;
    CorNode* idP   = (eP->type == CorObject) ? corTreeLookup(eP, "id") : NULL;

    if ((idP == NULL) || (idP->type != CorString) || (authEntityAttrsFilter(eP) == false))   // left out, or what of it may not be read
      corTreeChildRemove(arrayP, eP);

    eP = nextP;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// authEntityDeny - the bound request's answer when its one entity may not: a 403
//
void authEntityDeny(const char* detail)
{
  AuthAnswer ans;

  memset(&ans, 0, sizeof(ans));
  ans.detail = detail;
  answer(AuthDeny, &ans);
}



// -----------------------------------------------------------------------------
//
// authRelease -
//
void authRelease(void)
{
  AuthState* stateP = (AuthState*) corNgsild.authP;

  if ((loaded == false) || (stateP == NULL) || (stateP->released == true))
    return;

  stateP->released = true;

  if ((driver.release != NULL) && (stateP->identity != NULL))
    driver.release(stateP->identity);

  stateP->identity = NULL;
}



// -----------------------------------------------------------------------------
//
// authTrustNext -
//
void authTrustNext(void)
{
  if (loaded == true)
    trustNext = true;
}



// -----------------------------------------------------------------------------
//
// authTrustClear -
//
void authTrustClear(void)
{
  trustNext = false;
}



// -----------------------------------------------------------------------------
//
// authVersionInfo -
//
const char* authVersionInfo(void)
{
  if (loaded == false)
    return NULL;

  if (driver.versionInfo != NULL)
    return driver.versionInfo();

  static char info[128];

  snprintf(info, sizeof(info), "%s %s", (driver.name != NULL) ? driver.name : "?", (driver.version != NULL) ? driver.version : "");
  return info;
}



// -----------------------------------------------------------------------------
//
// authClose -
//
void authClose(void)
{
  if ((loaded == true) && (driver.close != NULL))
    driver.close();
}
