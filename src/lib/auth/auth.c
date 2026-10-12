//
// FILE            auth.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdio.h>                                   // snprintf
#include <string.h>                                  // memset, strcasecmp

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
#include "corNgsild/LdOp.h"                          // LdOp, ldOpToString

#include "db/DbDriver.h"                             // db, DB_OK
#include "db/Tenant.h"                               // Tenant
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
                          LdOpQueryEntities | LdOpBatchQuery)



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
} AuthState;



// -----------------------------------------------------------------------------
//
// trustNext - authTrustNext's mark, consumed by the next admission on this thread. A thread-local and not
// request state on purpose: the request it marks does not exist yet when it is set.
//
static __thread bool trustNext = false;



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
static void requestFill(AuthRequest* reqP, bool withTenant)
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
    reqP->expand        = termExpand;
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
    corRestOutHeaderAdd("WWW-Authenticate", (answerP->wwwAuthenticate != NULL) ? answerP->wwwAuthenticate : "Bearer");
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

  bodyKeep(stateP);
  pathKeep(stateP);

  AuthRequest  req;
  AuthAnswer   ans;
  void*        identity = NULL;

  requestFill(&req, false);
  memset(&ans, 0, sizeof(ans));

  AuthVerdict verdict = driver.authenticate(&req, &identity, &ans);

  stateP->identity = identity;

  if (verdict == AuthAllow)
    return true;

  answer(verdict, &ans);
  return false;
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
  if ((stateP == NULL) || (stateP->trusted == true))
    return true;

  AuthRequest  req;
  AuthAnswer   ans;

  requestFill(&req, true);
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
bool authEntityAllowed(const char* id, const char* const* typeV, int types, bool stored, const char* const* attrV, int attrs, const char** detailP)
{
  if (authEntityWanted() == false)
    return true;

  AuthState*   stateP = (AuthState*) corNgsild.authP;
  AuthRequest  req;
  AuthAnswer   ans;
  AuthEntity   entity;

  requestFill(&req, true);
  memset(&ans, 0, sizeof(ans));

  entity.id     = id;
  entity.typeV  = typeV;
  entity.types  = types;
  entity.stored = stored;
  entity.attrV  = attrV;
  entity.attrs  = attrs;

  AuthVerdict verdict = driver.entity(&req, stateP->identity, &entity, &ans);

  if (verdict == AuthAllow)
    return true;

  if (detailP != NULL)
    *detailP = (ans.detail != NULL) ? corAllocStrdup(&corRest.kalloc, ans.detail) : "not allowed";

  return false;
}



// -----------------------------------------------------------------------------
//
// authEntityTreeAllowed - authEntityAllowed for an entity whose types are a tree: "T" or ["T1", "T2"],
// expanded
//
bool authEntityTreeAllowed(const char* id, CorNode* typeP, bool stored, const char** detailP)
{
  if (authEntityWanted() == false)
    return true;

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

    typeV = (const char**) corAlloc(&corRest.kalloc, (n + 1) * sizeof(char*));
    if (typeV == NULL)
      return false;

    for (CorNode* tP = typeP->value.head; tP != NULL; tP = tP->next)
    {
      if (tP->type == CorString)
        typeV[types++] = tP->value.s;
    }
  }

  return authEntityAllowed(id, typeV, types, stored, NULL, 0, detailP);
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
  CorNode* entityP = NULL;
  CorNode* typeP   = NULL;

  if ((corNgsild.tenantP != NULL) && (db.entityRetrieve != NULL) && (db.entityRetrieve((Tenant*) corNgsild.tenantP, id, &entityP) == DB_OK) && (entityP != NULL))
    typeP = corTreeLookup(entityP, "type");

  const char* detail = NULL;

  if (authEntityTreeAllowed(id, typeP, true, &detail) == true)
    return true;

  authEntityDeny(detail);
  return false;
}



// -----------------------------------------------------------------------------
//
// authEntityBatchAllowed -
//
bool authEntityBatchAllowed(const char* id, CorNode* bodyTypeP, bool lookup, const char** detailP)
{
  if (authEntityWanted() == false)
    return true;

  CorNode* entityP = NULL;

  if ((lookup == true) && (corNgsild.tenantP != NULL) && (db.entityRetrieve != NULL) &&
      (db.entityRetrieve((Tenant*) corNgsild.tenantP, id, &entityP) == DB_OK) && (entityP != NULL))
    return authEntityTreeAllowed(id, corTreeLookup(entityP, "type"), true, detailP);

  return authEntityTreeAllowed(id, bodyTypeP, false, detailP);
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

    if ((idP == NULL) || (idP->type != CorString) || (authEntityTreeAllowed(idP->value.s, corTreeLookup(eP, "type"), true, NULL) == false))
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
