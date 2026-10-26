//
// FILE            oidc.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// oidc.so - an OpenID provider's access tokens (Keycloak, Auth0, Entra ID, ...), inside the broker
// (doc/authorization.md). Simpler than fdsc.so, on purpose:
//
//   - the access token: a JWT the provider signed (RS256 / ES256), checked against its keys - its JWK Set,
//     found through its discovery document (<issuer>/.well-known/openid-configuration); iss the provider,
//     exp, nbf, and aud when --oidcAudience is given. A 401 without a valid one.
//   - what it may: two roles, read out of one claim (Keycloak's realm roles by default) - the read role
//     reads, the write role reads and writes - and the tenants it may use, out of another (tenants: a
//     token without it may use the default tenant only). A 403 for what the token does not cover.
//
// No policies, no per-entity decisions: every request is decided at its admission, before the broker
// fetches a @context for it. A subscription's notifications are decided for its owner - who made it, or
// last updated it - as a whole (notify, ABI 5): the owner's roles and tenants, kept with the subscription,
// hold for --oidcOwnerMaxAge; then the notifications stop until a valid token updates the subscription. The keys are fetched by a thread of this plugin; a request never waits for
// the network. Until they are there every request is a 503 (fail closed).
//
#include <pthread.h>                                 // pthread_*
#include <stdbool.h>                                 // bool
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // malloc, calloc, free
#include <string.h>                                  // strcmp, strlen, strncasecmp, strchr, strdup, strncpy
#include <time.h>                                    // time, clock_gettime

#include "corAlloc/CorAlloc.h"                       // CorAlloc
#include "corAlloc/corAlloc.h"                       // corAllocStrdup
#include "corAlloc/corAllocBufferInit.h"             // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"            // corAllocBufferReset
#include "corArgs/corArgs.h"                         // CorArg, CORARGS_END
#include "corJson/CorJson.h"                         // CorJson
#include "corJson/corJsonCreate.h"                   // corJsonCreate
#include "corJson/corJsonParse.h"                    // corJsonParse
#include "corBase/corUriPasswordMask.h"               // corUriPasswordMask
#include "corLog/corLog.h"                           // COR_I, COR_W
#include "corNgsild/LdOp.h"                          // LdOpBatchQuery, LdOpQueryTemporal, LdOpQueryEntityTimepoint
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeLookup.h"                   // corTreeLookup

#include "plugin/AuthDriver.h"                       // AuthDriver, AuthRequest, AuthAnswer, AuthVerdict

#include "authFetch.h"                               // authFetch
#include "authJwt.h"                                 // authJwtVerify, AuthJwtExpect, authJwtCheckString
#include "authKeys.h"                                // authKeysLoad, authKeysCount, authKeysRelease
#include "authTokenCache.h"                          // authTokenCacheInit, authTokenCacheGet, authTokenCacheAdd, authTokenCacheClear



#define OIDC_VERSION  "0.1.0"
#define _vp           (void*)



// -----------------------------------------------------------------------------
//
// The options
//
static char*  issuer        = NULL;                  // the iss a token must have - and, without --oidcDiscovery / --oidcJwks, where the discovery document is
static char*  discovery     = NULL;                  // the discovery document, when the broker reaches the provider by another name than its issuer's
static char*  jwks          = NULL;                  // the JWK Set itself (a URL or a file) - instead of discovery
static char*  audience      = NULL;                  // an aud a token must have - NULL: not checked
static char*  rolesClaim    = NULL;                  // where the roles are: a claim's path, dot-separated
static char*  readRole      = NULL;                  // the role that reads
static char*  writeRole     = NULL;                  // the role that reads and writes
static int    leeway        = 60;                    // seconds of clock skew on exp / nbf
static int    refresh       = 300;                   // seconds between two fetches of the keys
static bool   anonymousRead = false;                 // a read without a token is let in
static char*  tenantsClaim  = NULL;                  // where the tenants a token may use are: a claim's path, dot-separated
static bool   anyTenant     = false;                 // no tenant check: a token may use every tenant
static int    ownerMaxAge   = 86400;                 // seconds a subscription's owner, as kept, holds

static CorArg oidcArgs[] =
{
  { "--oidcIssuer",        "-oidcIssuer",        CorArgString, _vp &issuer,        CorArgOpt, _vp NULL,                  NULL,      NULL,      "the OpenID provider's issuer - the iss a token must have (https://keycloak.example.org/realms/city); its discovery document is <issuer>/.well-known/openid-configuration" },
  { "--oidcDiscovery",     "-oidcDiscovery",     CorArgString, _vp &discovery,     CorArgOpt, _vp NULL,                  NULL,      NULL,      "the discovery document's URL, when the broker reaches the provider by another name than the issuer's (http://keycloak:8080/realms/city/.well-known/openid-configuration)" },
  { "--oidcJwks",          "-oidcJwks",          CorArgString, _vp &jwks,          CorArgOpt, _vp NULL,                  NULL,      NULL,      "the provider's JWK Set - a URL or a file - instead of the discovery document" },
  { "--oidcAudience",      "-oidcAudience",      CorArgString, _vp &audience,      CorArgOpt, _vp NULL,                  NULL,      NULL,      "an audience (aud) a token must have; default: not checked" },
  { "--oidcRolesClaim",    "-oidcRolesClaim",    CorArgString, _vp &rolesClaim,    CorArgOpt, _vp "realm_access.roles",  NULL,      NULL,      "the claim holding the roles - a path, dot-separated; an array of strings, or a string of space-separated ones (scope)" },
  { "--oidcReadRole",      "-oidcReadRole",      CorArgString, _vp &readRole,      CorArgOpt, _vp "ngsild-read",         NULL,      NULL,      "the role that reads: GET, HEAD, OPTIONS and the query operations (POST .../entityOperations/query)" },
  { "--oidcWriteRole",     "-oidcWriteRole",     CorArgString, _vp &writeRole,     CorArgOpt, _vp "ngsild-write",        NULL,      NULL,      "the role that reads and writes: every request" },
  { "--oidcLeeway",        "-oidcLeeway",        CorArgInt,    _vp &leeway,        CorArgOpt, _vp 60,                    _vp 0,     _vp 600,   "seconds of clock skew allowed on a token's exp and nbf" },
  { "--oidcRefresh",       "-oidcRefresh",       CorArgInt,    _vp &refresh,       CorArgOpt, _vp 300,                   _vp 1,     _vp 86400, "seconds between two fetches of the provider's keys - and at once when a token names a key the set lacks" },
  { "--oidcAnonymousRead", "-oidcAnonymousRead", CorArgBool,   _vp &anonymousRead, CorArgOpt, _vp false,                 _vp false, _vp true,  "a read without a token is let in - in every tenant; a write without one is still a 401" },
  { "--oidcTenantsClaim",  "-oidcTenantsClaim",  CorArgString, _vp &tenantsClaim,  CorArgOpt, _vp "tenants",             NULL,      NULL,      "the claim holding the tenants a token may use - a path, dot-separated; an array of strings or a string of space-separated ones; 'default' is the default tenant (no NGSILD-Tenant), '*' every tenant. A token without it: the default tenant only" },
  { "--oidcAnyTenant",     "-oidcAnyTenant",     CorArgBool,   _vp &anyTenant,     CorArgOpt, _vp false,                 _vp false, _vp true,  "no tenant check: a token may use every tenant" },
  { "--oidcOwnerMaxAge",   "-oidcOwnerMaxAge",   CorArgInt,    _vp &ownerMaxAge,   CorArgOpt, _vp 86400,                 _vp 1,     _vp 31536000, "seconds a subscription's owner - the roles and tenants of the token that made or last updated it - holds: after that its notifications stop until a valid token updates the subscription" },
  CORARGS_END
};



// -----------------------------------------------------------------------------
//
// OidcIdentity - what a token may do; made once per token, kept in the token cache, shared by the requests
// that send it (referenced)
//
#define RIGHT_READ   1
#define RIGHT_WRITE  2

typedef struct OidcIdentity
{
  int        refs;
  int        rights;                                 // RIGHT_READ | RIGHT_WRITE
  long long  exp;                                    // the token's exp, nbf (0: none) - checked at every use of a cached identity
  long long  nbf;
  long long  ownerUntil;                             // a subscription's owner (identityRestore): when it stops holding; 0: a request's
  bool       allTenants;                             // '*' in the tenants claim
  int        tenants;                                // the tenants it may use, in tenantBuf: each one NUL-terminated, one after the other
  char       tenantBuf[];
} OidcIdentity;

static void identityRef(void* identity)
{
  __atomic_add_fetch(&((OidcIdentity*) identity)->refs, 1, __ATOMIC_RELAXED);
}

static void identityUnref(void* identity)
{
  OidcIdentity* idP = (OidcIdentity*) identity;

  if ((idP != NULL) && (__atomic_sub_fetch(&idP->refs, 1, __ATOMIC_ACQ_REL) == 0))
    free(idP);
}



// -----------------------------------------------------------------------------
//
// The refresh thread
//
static pthread_t        refreshThread;
static bool             refreshRunning = false;
static pthread_mutex_t  refreshMutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   refreshCond    = PTHREAD_COND_INITIALIZER;
static bool             refreshStop    = false;
static bool             refreshAsked   = false;     // a token named a key the set does not have
static time_t           keysFetchedAt  = 0;
static char*            discoveryUrl   = NULL;      // where the discovery document is - malloc'd
static char*            jwksUrl        = NULL;      // from the discovery document - malloc'd



// -----------------------------------------------------------------------------
//
// discoveryRead - the discovery document's jwks_uri; its issuer must be --oidcIssuer (OpenID Connect
// Discovery 1.0 § 4.3) - a document of another issuer is not this provider's
//
static bool discoveryRead(char* errorBuf, int errorBufSize)
{
  CorAlloc ka;
  char     buf[4096];

  corAllocBufferInit(&ka, buf, sizeof(buf), 16384, NULL, "oidc discovery");

  char* text = authFetch(discoveryUrl, &ka, errorBuf, errorBufSize);
  bool  ok   = false;
  char  shown[512];                                   // the URL as an error shows it: its password "***"

  if (text != NULL)
  {
    CorJson cj;
    corJsonCreate(&cj, &ka);

    CorNode* rootP = corJsonParse(&cj, text);
    CorNode* uriP  = (rootP != NULL) ? corTreeLookup(rootP, "jwks_uri") : NULL;
    CorNode* issP  = (rootP != NULL) ? corTreeLookup(rootP, "issuer")   : NULL;

    if ((uriP == NULL) || (uriP->type != CorString))
      snprintf(errorBuf, errorBufSize, "%s: no jwks_uri", corUriPasswordMask(discoveryUrl, shown, sizeof(shown)));
    else if ((issP == NULL) || (issP->type != CorString) || (strcmp(issP->value.s, issuer) != 0))
      snprintf(errorBuf, errorBufSize, "%s: its issuer is '%s', not --oidcIssuer's '%s'", corUriPasswordMask(discoveryUrl, shown, sizeof(shown)), ((issP != NULL) && (issP->type == CorString)) ? issP->value.s : "", issuer);
    else
    {
      free(jwksUrl);
      jwksUrl = strdup(uriP->value.s);
      ok      = (jwksUrl != NULL);
    }
  }

  corAllocBufferReset(&ka, false);
  return ok;
}



// -----------------------------------------------------------------------------
//
// keysFetch -
//
static void keysFetch(void)
{
  char errorBuf[512] = "";

  if ((jwks == NULL) && (discoveryRead(errorBuf, sizeof(errorBuf)) == false))
  {
    COR_W("oidc: the provider's discovery document: %s", errorBuf);
    return;
  }

  CorAlloc    ka;
  char        buf[8192];
  const char* where = (jwks != NULL) ? jwks : jwksUrl;

  corAllocBufferInit(&ka, buf, sizeof(buf), 16384, NULL, "oidc keys");

  char* text = authFetch(where, &ka, errorBuf, sizeof(errorBuf));
  char  shown[512];

  if ((text == NULL) || (authKeysLoad(text, authTokenCacheClear, errorBuf, sizeof(errorBuf)) == false))
    COR_W("oidc: the provider's keys (%s): %s", corUriPasswordMask(where, shown, sizeof(shown)), errorBuf);
  else
    __atomic_store_n(&keysFetchedAt, time(NULL), __ATOMIC_RELAXED);

  corAllocBufferReset(&ka, false);
}



// -----------------------------------------------------------------------------
//
// refreshLoop - the keys now, and every --oidcRefresh seconds; every second until there are some (the
// provider may start after the broker); sooner when a token names a key the set lacks (the provider
// rotated its keys), but not more than once a second
//
static void* refreshLoop(void* arg)
{
  (void) arg;

  while (true)
  {
    keysFetch();

    pthread_mutex_lock(&refreshMutex);

    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += (authKeysCount() == 0) ? 1 : refresh;

    while ((refreshStop == false) && (refreshAsked == false))
    {
      if (pthread_cond_timedwait(&refreshCond, &refreshMutex, &until) != 0)
        break;
    }

    bool stop = refreshStop;

    refreshAsked = false;
    pthread_mutex_unlock(&refreshMutex);

    if (stop == true)
      break;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// refreshAsk - a token named an unknown key: refetch the keys, unless that was done this second
//
static void refreshAsk(void)
{
  if (time(NULL) - __atomic_load_n(&keysFetchedAt, __ATOMIC_RELAXED) < 1)
    return;

  pthread_mutex_lock(&refreshMutex);
  refreshAsked = true;
  pthread_cond_signal(&refreshCond);
  pthread_mutex_unlock(&refreshMutex);
}



// -----------------------------------------------------------------------------
//
// oidcInit -
//
static bool oidcInit(char* errorBuf, int errorBufSize)
{
  authTokenCacheInit(identityRef, identityUnref);

  if (issuer == NULL)
  {
    snprintf(errorBuf, errorBufSize, "no provider: --oidcIssuer (a token's iss is always checked)");
    return false;
  }

  if ((discovery != NULL) && (jwks != NULL))
  {
    snprintf(errorBuf, errorBufSize, "--oidcDiscovery and --oidcJwks: one or the other");
    return false;
  }

  if ((rolesClaim[0] == 0) || (readRole[0] == 0) || (writeRole[0] == 0) || (tenantsClaim[0] == 0))
  {
    snprintf(errorBuf, errorBufSize, "--oidcRolesClaim, --oidcReadRole, --oidcWriteRole, --oidcTenantsClaim: none may be empty");
    return false;
  }

  if (discovery != NULL)
    discoveryUrl = strdup(discovery);
  else
  {
    int   len   = strlen(issuer);
    int   size  = len + 64;
    char* url   = (char*) malloc(size);

    if (url != NULL)
      snprintf(url, size, "%.*s/.well-known/openid-configuration", (len > 0) && (issuer[len - 1] == '/') ? len - 1 : len, issuer);
    discoveryUrl = url;
  }

  if (discoveryUrl == NULL)
  {
    snprintf(errorBuf, errorBufSize, "out of memory");
    return false;
  }

  if (pthread_create(&refreshThread, NULL, refreshLoop, NULL) != 0)
  {
    snprintf(errorBuf, errorBufSize, "cannot start the refresh thread");
    return false;
  }

  refreshRunning = true;

  if (audience == NULL)
    COR_W("oidc: --oidcAudience not set - a token issued for any client of the provider is taken");

  char issuerShown[512];
  char keysShown[512];
  COR_I("oidc: tokens of %s, keys from %s - roles in '%s': '%s' reads, '%s' reads and writes; tenants in '%s'%s%s", corUriPasswordMask(issuer, issuerShown, sizeof(issuerShown)), corUriPasswordMask((jwks != NULL) ? jwks : discoveryUrl, keysShown, sizeof(keysShown)), rolesClaim, readRole, writeRole, tenantsClaim, (anyTenant == true) ? " - not checked" : "", (anonymousRead == true) ? "; reads without a token" : "");
  return true;
}



// -----------------------------------------------------------------------------
//
// isRead - the request reads: GET, HEAD, OPTIONS, and the three query operations that are POSTs
//
static bool isRead(const AuthRequest* reqP)
{
  const char* m = reqP->method;

  if ((strcmp(m, "GET") == 0) || (strcmp(m, "HEAD") == 0) || (strcmp(m, "OPTIONS") == 0))
    return true;

  return (strcmp(m, "POST") == 0) && ((reqP->op & (LdOpBatchQuery | LdOpQueryTemporal | LdOpQueryEntityTimepoint)) != 0);
}



// -----------------------------------------------------------------------------
//
// claimPath - a claim by its dot-separated path ("realm_access.roles"); NULL: not there
//
static CorNode* claimPath(CorNode* claimsP, const char* path)
{
  char     name[256];
  CorNode* nodeP = claimsP;

  while ((nodeP != NULL) && (*path != 0))
  {
    const char* dot = strchr(path, '.');
    int         len = (dot != NULL) ? (int) (dot - path) : (int) strlen(path);

    if ((len == 0) || (len >= (int) sizeof(name)) || (nodeP->type != CorObject))
      return NULL;

    memcpy(name, path, len);
    name[len] = 0;
    nodeP     = corTreeLookup(nodeP, name);
    path     += (dot != NULL) ? len + 1 : len;
  }

  return nodeP;
}



// -----------------------------------------------------------------------------
//
// hasRole - the roles claim holds the role: an array of strings, or a string of space-separated ones
//
static bool hasRole(CorNode* rolesP, const char* role)
{
  if (rolesP == NULL)
    return false;

  if (rolesP->type == CorArray)
  {
    for (CorNode* rP = rolesP->value.head; rP != NULL; rP = rP->next)
    {
      if ((rP->type == CorString) && (strcmp(rP->value.s, role) == 0))
        return true;
    }

    return false;
  }

  if (rolesP->type != CorString)
    return false;

  int         len = strlen(role);
  const char* s   = rolesP->value.s;

  while (*s != 0)
  {
    while (*s == ' ')
      ++s;

    const char* end = s;
    while ((*end != 0) && (*end != ' '))
      ++end;

    if ((end - s == len) && (strncmp(s, role, len) == 0))
      return true;

    s = end;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// tenantsCopy - the tenants claim's values, NUL-terminated one after the other, into out (NULL: only the
// length); the bytes they take. '*' among them: *allP
//
static int tenantsCopy(CorNode* tenantsP, char* out, int* countP, bool* allP)
{
  int len = 0;

  *countP = 0;
  *allP   = false;

  if (tenantsP == NULL)
    return 0;

  if (tenantsP->type == CorArray)
  {
    for (CorNode* tP = tenantsP->value.head; tP != NULL; tP = tP->next)
    {
      if ((tP->type != CorString) || (tP->value.s[0] == 0))
        continue;

      int n = strlen(tP->value.s) + 1;

      if (strcmp(tP->value.s, "*") == 0)
        *allP = true;
      if (out != NULL)
        memcpy(&out[len], tP->value.s, n);

      len     += n;
      *countP += 1;
    }

    return len;
  }

  if (tenantsP->type != CorString)
    return 0;

  const char* s = tenantsP->value.s;

  while (*s != 0)
  {
    while (*s == ' ')
      ++s;

    const char* end = s;
    while ((*end != 0) && (*end != ' '))
      ++end;

    int n = end - s;

    if (n > 0)
    {
      if ((n == 1) && (*s == '*'))
        *allP = true;

      if (out != NULL)
      {
        memcpy(&out[len], s, n);
        out[len + n] = 0;
      }

      len     += n + 1;
      *countP += 1;
    }

    s = end;
  }

  return len;
}



// -----------------------------------------------------------------------------
//
// tenantAllowed - the request's tenant (NULL: the default tenant, 'default' in the claim) is one the token
// may use
//
static bool tenantAllowed(const OidcIdentity* idP, const char* tenant)
{
  if ((anyTenant == true) || (idP->allTenants == true))
    return true;

  const char* name = (tenant != NULL) ? tenant : "default";
  const char* s    = idP->tenantBuf;

  for (int i = 0; i < idP->tenants; i++)
  {
    if (strcmp(s, name) == 0)
      return true;

    s += strlen(s) + 1;
  }

  //
  // A token whose claim names no tenant at all: the default tenant only
  //
  return (idP->tenants == 0) && (tenant == NULL);
}



// -----------------------------------------------------------------------------
//
// numericClaim -
//
static long long numericClaim(CorNode* claimsP, const char* name)
{
  CorNode* nodeP = corTreeLookup(claimsP, name);

  if (nodeP == NULL)
    return 0;
  if (nodeP->type == CorInt)
    return nodeP->value.i;
  if (nodeP->type == CorFloat)
    return (long long) nodeP->value.f;

  return 0;
}



// -----------------------------------------------------------------------------
//
// bearer - the token of an Authorization: Bearer header (RFC 6750 § 2.1), NULL: none
//
static const char* bearer(const AuthRequest* reqP)
{
  const char* value = reqP->header("Authorization");

  if ((value == NULL) || (strncasecmp(value, "Bearer ", 7) != 0))
    return NULL;

  value += 7;
  while (*value == ' ')
    ++value;

  return (*value != 0) ? value : NULL;
}



// -----------------------------------------------------------------------------
//
// invalidToken - a 401 for a token that does not hold, said in WWW-Authenticate too (RFC 6750 § 3)
//
static AuthVerdict invalidToken(AuthJwtCheck check, AuthAnswer* answerP)
{
  static __thread char wwwAuth[160];

  snprintf(wwwAuth, sizeof(wwwAuth), "Bearer error=\"invalid_token\", error_description=\"%s\"", authJwtCheckString(check));

  answerP->detail          = authJwtCheckString(check);
  answerP->wwwAuthenticate = wwwAuth;
  return AuthUnauthenticated;
}



// -----------------------------------------------------------------------------
//
// oidcAuthenticate -
//
static AuthVerdict oidcAuthenticate(const AuthRequest* reqP, void** identityP, AuthAnswer* answerP)
{
  const char* token = bearer(reqP);

  if (token == NULL)
  {
    if ((anonymousRead == true) && (isRead(reqP) == true))
    {
      *identityP = NULL;                             // no one: authorize lets a read through
      return AuthAllow;
    }

    answerP->detail = "no access token (Authorization: Bearer)";
    return AuthUnauthenticated;
  }

  if (authKeysCount() == 0)
  {
    answerP->detail = "the provider's keys are not loaded yet";
    return AuthUnavailable;
  }

  //
  // A token checked before: what it may do, already decided - only its time is checked again
  //
  int           tokenLen = strlen(token);
  long long     now      = (long long) time(NULL);
  OidcIdentity* idP      = (OidcIdentity*) authTokenCacheGet(token, tokenLen);

  if (idP != NULL)
  {
    if (((idP->exp != 0) && (now > idP->exp + leeway)) || ((idP->nbf != 0) && (now + leeway < idP->nbf)))
    {
      identityUnref(idP);
      return invalidToken(AuthJwtExpired, answerP);
    }

    *identityP = idP;                                // the cache's Get gave a reference: release() drops it
    return AuthAllow;
  }

  unsigned int generation = authTokenCacheGeneration();   // before the check: a key set that changes meanwhile keeps it out of the cache
  CorAlloc     ka;
  char         buf[4096];

  corAllocBufferInit(&ka, buf, sizeof(buf), 8192, NULL, "oidc token");

  AuthJwtExpect expect = { issuer, audience, now, leeway };
  CorNode*      claimsP = NULL;
  AuthJwtCheck  check   = authJwtVerify(token, &expect, &ka, &claimsP);

  if (check != AuthJwtOk)
  {
    corAllocBufferReset(&ka, false);

    if (check == AuthJwtUnknownKey)
      refreshAsk();

    return invalidToken(check, answerP);
  }

  CorNode* tenantsP = (anyTenant == false) ? claimPath(claimsP, tenantsClaim) : NULL;
  int      tenants   = 0;
  bool     all       = false;
  int      tenantLen = tenantsCopy(tenantsP, NULL, &tenants, &all);

  idP = (OidcIdentity*) calloc(1, sizeof(OidcIdentity) + tenantLen);

  if (idP == NULL)
  {
    corAllocBufferReset(&ka, false);
    answerP->detail = "out of memory";
    return AuthUnavailable;
  }

  tenantsCopy(tenantsP, idP->tenantBuf, &idP->tenants, &idP->allTenants);

  CorNode* rolesP = claimPath(claimsP, rolesClaim);

  idP->refs = 1;                                     // the request's
  idP->exp  = numericClaim(claimsP, "exp");
  idP->nbf  = numericClaim(claimsP, "nbf");

  if (hasRole(rolesP, writeRole) == true)
    idP->rights = RIGHT_READ | RIGHT_WRITE;
  else if (hasRole(rolesP, readRole) == true)
    idP->rights = RIGHT_READ;

  corAllocBufferReset(&ka, false);

  authTokenCacheAdd(token, tokenLen, idP, generation);
  *identityP = idP;
  return AuthAllow;
}



// -----------------------------------------------------------------------------
//
// oidcAuthorize - the token's roles against what the request does
//
static AuthVerdict oidcAuthorize(const AuthRequest* reqP, void* identity, AuthAnswer* answerP)
{
  OidcIdentity* idP  = (OidcIdentity*) identity;
  bool          read = isRead(reqP);

  if (idP == NULL)                                   // no token: authenticate let only a read through
  {
    if (read == true)
      return AuthAllow;

    answerP->detail = "no access token (Authorization: Bearer)";
    return AuthDeny;
  }

  static __thread char detail[384];

  if (tenantAllowed(idP, reqP->tenant) == false)
  {
    snprintf(detail, sizeof(detail), "the token may not use the tenant '%s' (claim '%s')", (reqP->tenant != NULL) ? reqP->tenant : "default", tenantsClaim);
    answerP->detail = detail;
    return AuthDeny;
  }

  if ((idP->rights & (read ? RIGHT_READ : RIGHT_WRITE)) != 0)
    return AuthAllow;

  if (read == true)
    snprintf(detail, sizeof(detail), "the token has neither role '%s' nor '%s' (claim '%s')", readRole, writeRole, rolesClaim);
  else
    snprintf(detail, sizeof(detail), "a write needs the role '%s' (claim '%s')", writeRole, rolesClaim);

  answerP->detail = detail;
  return AuthDeny;
}



// -----------------------------------------------------------------------------
//
// oidcRelease -
//
static void oidcRelease(void* identity)
{
  identityUnref(identity);
}



// -----------------------------------------------------------------------------
//
// oidcIdentitySave - a subscription's owner, as text: "oidc1 <rights> <until> <allTenants>" and a line per
// tenant. Kept with the subscription; it holds for --oidcOwnerMaxAge from now - the token's own exp is
// not what it lasts (a token lives minutes, a subscription months)
//
static char* oidcIdentitySave(void* identity)
{
  OidcIdentity* idP = (OidcIdentity*) identity;

  if (idP == NULL)                                   // no token (--oidcAnonymousRead): no owner - and a write needs one
    return NULL;

  int   size = 64;
  const char* s = idP->tenantBuf;

  for (int i = 0; i < idP->tenants; i++)
  {
    size += strlen(s) + 1;
    s    += strlen(s) + 1;
  }

  char* text = (char*) malloc(size);

  if (text == NULL)
    return NULL;

  int n = snprintf(text, size, "oidc1 %d %lld %d", idP->rights, (long long) time(NULL) + ownerMaxAge, (idP->allTenants == true) ? 1 : 0);

  s = idP->tenantBuf;
  for (int i = 0; i < idP->tenants; i++)
  {
    n += snprintf(&text[n], size - n, "\n%s", s);
    s += strlen(s) + 1;
  }

  return text;
}



// -----------------------------------------------------------------------------
//
// oidcIdentityRestore - a kept owner; NULL: not one of this plugin's, or no longer holding
//
static void* oidcIdentityRestore(const char* saved)
{
  int       rights = 0;
  long long until  = 0;
  int       all    = 0;
  int       used   = 0;

  if ((saved == NULL) || (sscanf(saved, "oidc1 %d %lld %d%n", &rights, &until, &all, &used) != 3))
    return NULL;

  if ((long long) time(NULL) > until)
    return NULL;

  const char* tenantText = &saved[used];             // "\n<tenant>" ...
  int         len        = strlen(tenantText);
  OidcIdentity* idP      = (OidcIdentity*) calloc(1, sizeof(OidcIdentity) + len + 1);

  if (idP == NULL)
    return NULL;

  idP->refs       = 1;
  idP->rights     = rights & (RIGHT_READ | RIGHT_WRITE);
  idP->ownerUntil = until;
  idP->allTenants = (all == 1);

  //
  // The tenants, NUL-terminated one after the other
  //
  int o = 0;

  for (const char* s = tenantText; *s != 0; )
  {
    if (*s == '\n')
    {
      ++s;
      continue;
    }

    const char* end = strchr(s, '\n');
    int         n   = (end != NULL) ? (int) (end - s) : (int) strlen(s);

    memcpy(&idP->tenantBuf[o], s, n);
    idP->tenantBuf[o + n] = 0;
    o            += n + 1;
    idP->tenants += 1;
    s            += n;
  }

  return idP;
}



// -----------------------------------------------------------------------------
//
// oidcNotify - may the subscription's owner have its notifications: it reads, in the subscription's
// tenant, and it still holds (the broker keeps a restored owner a while)
//
static AuthVerdict oidcNotify(void* identity, const char* tenant)
{
  OidcIdentity* idP = (OidcIdentity*) identity;

  if ((idP == NULL) || ((idP->rights & RIGHT_READ) == 0))
    return AuthDeny;

  if ((idP->ownerUntil != 0) && ((long long) time(NULL) > idP->ownerUntil))
    return AuthDeny;

  return (tenantAllowed(idP, tenant) == true) ? AuthAllow : AuthDeny;
}



// -----------------------------------------------------------------------------
//
// jsonAdd - s as a JSON string, quotes included, appended at out[*nP] (room checked by the caller: 6 a byte)
//
static void jsonAdd(char* out, int* nP, const char* s)
{
  int n = *nP;

  out[n++] = '"';
  for (const unsigned char* c = (const unsigned char*) s; *c != 0; c++)
  {
    if ((*c == '"') || (*c == '\\'))
    {
      out[n++] = '\\';
      out[n++] = *c;
    }
    else if (*c < 0x20)
      n += sprintf(&out[n], "\\u%04x", *c);
    else
      out[n++] = *c;
  }
  out[n++] = '"';
  out[n]   = 0;
  *nP      = n;
}



// -----------------------------------------------------------------------------
//
// oidcResourceMetadata - RFC 9728's authorization_servers (the issuer), and what c3 and other clients need to
// log in and to know what the token is read for
//
static char* oidcResourceMetadata(void)
{
  int   size = 256 + 6 * (strlen(issuer) + strlen(readRole) + strlen(writeRole) + strlen(rolesClaim) + strlen(tenantsClaim));
  char* text = (char*) malloc(size);
  int   n    = 0;

  if (text == NULL)
    return NULL;

  n += sprintf(&text[n], "{\"authorization_servers\": [");
  jsonAdd(text, &n, issuer);
  n += sprintf(&text[n], "], \"scopes_supported\": [\"openid\"], \"coraine_auth\": \"oidc\", \"coraine_roles\": {\"claim\": ");
  jsonAdd(text, &n, rolesClaim);
  n += sprintf(&text[n], ", \"read\": ");
  jsonAdd(text, &n, readRole);
  n += sprintf(&text[n], ", \"write\": ");
  jsonAdd(text, &n, writeRole);
  n += sprintf(&text[n], "}, \"coraine_tenants\": ");
  if (anyTenant == true)
    n += sprintf(&text[n], "null");
  else
  {
    n += sprintf(&text[n], "{\"claim\": ");
    jsonAdd(text, &n, tenantsClaim);
    n += sprintf(&text[n], "}");
  }
  sprintf(&text[n], "}");
  return text;
}



// -----------------------------------------------------------------------------
//
// oidcVersionInfo -
//
static const char* oidcVersionInfo(void)
{
  static __thread char info[160];

  snprintf(info, sizeof(info), "oidc %s - %d keys", OIDC_VERSION, authKeysCount());
  return info;
}



// -----------------------------------------------------------------------------
//
// oidcClose -
//
static void oidcClose(void)
{
  if (refreshRunning == true)
  {
    pthread_mutex_lock(&refreshMutex);
    refreshStop = true;
    pthread_cond_signal(&refreshCond);
    pthread_mutex_unlock(&refreshMutex);

    pthread_join(refreshThread, NULL);
    refreshRunning = false;
  }

  authKeysRelease(authTokenCacheClear);
  free(discoveryUrl);
  free(jwksUrl);
  discoveryUrl = NULL;
  jwksUrl      = NULL;
}



// -----------------------------------------------------------------------------
//
// authRegister - the plugin's entry point
//
void authRegister(AuthDriver* driverP)
{
  int brokerAbi = driverP->abiVersion;               // the broker's - how much of the struct it has

  driverP->abiVersion   = (brokerAbi < AUTH_ABI_VERSION) ? brokerAbi : AUTH_ABI_VERSION;
  driverP->name         = "oidc";
  driverP->version      = OIDC_VERSION;
  driverP->args         = oidcArgs;
  driverP->init         = oidcInit;
  driverP->authenticate = oidcAuthenticate;
  driverP->authorize    = oidcAuthorize;
  driverP->release      = oidcRelease;
  driverP->versionInfo  = oidcVersionInfo;
  driverP->close        = oidcClose;

  //
  // Decided on the request alone - its method and operation, never its @context: before the broker fetches
  // a @context the client names
  //
  if (brokerAbi >= 3)
    driverP->authorizeBeforeContext = true;

  //
  // A subscription's notifications, decided for its owner as a whole - a broker older than ABI 5 cannot ask
  // that, and would keep owners it never asks about: then nothing is kept
  //
  if (brokerAbi >= 5)
  {
    driverP->identitySave    = oidcIdentitySave;
    driverP->identityRestore = oidcIdentityRestore;
    driverP->notify          = oidcNotify;
  }

  if (brokerAbi >= 6)
    driverP->resourceMetadata = oidcResourceMetadata;
}
