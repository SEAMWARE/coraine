//
// FILE            fdsc.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// fdsc.so - the FIWARE Data Space Connector's security, inside the broker (doc/authorization.md). It takes
// what the connector's provider side already has, unchanged:
//
//   - the access token VCVerifier issues for a Verifiable Presentation - a JWT, checked against the
//     verifier's keys (its JWK Set, found through the same discovery document APISIX's openid-connect is
//     given) - a 401 without a valid one, as APISIX answers;
//   - the ODRL policies of the ODRL-PAP, evaluated here on the parsed request - a 403 where OPA would deny.
//
// Keys and policies are fetched by a thread of this plugin and replaced whole; a request never waits for
// the network. Until both are there every request is a 503 (fail closed).
//
#include <pthread.h>                                 // pthread_*
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // malloc, free
#include <string.h>                                  // strcmp, strlen, strncasecmp
#include <time.h>                                    // time, clock_gettime

#include "corAlloc/CorAlloc.h"                       // CorAlloc
#include "corAlloc/corAlloc.h"                       // corAllocStrdup
#include "corAlloc/corAllocBufferInit.h"             // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"            // corAllocBufferReset
#include "corArgs/corArgs.h"                         // CorArg, CORARGS_END
#include "corJson/CorJson.h"                         // CorJson
#include "corJson/corJsonCreate.h"                   // corJsonCreate
#include "corJson/corJsonParse.h"                    // corJsonParse
#include "corLog/corLog.h"                           // COR_I, COR_W, COR_E
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeLookup.h"                   // corTreeLookup

#include "plugin/AuthDriver.h"                       // AuthDriver, AuthRequest, AuthAnswer, AuthVerdict

#include "fdscFetch.h"                               // fdscFetch
#include "fdscIdentity.h"                            // FdscIdentity
#include "fdscJwt.h"                                 // fdscJwtVerify, FdscJwtExpect
#include "fdscKeys.h"                                // fdscKeysLoad, fdscKeysCount
#include "fdscPolicy.h"                              // fdscPolicyLoad, fdscPolicyDecide, fdscPolicyCount



#define _vp (void*)
#define FDSC_VERSION  "1.0.0"



// -----------------------------------------------------------------------------
//
// The options
//
static char*  discovery     = NULL;                  // the verifier's OpenID discovery document - APISIX openid-connect's "discovery"
static char*  jwks          = NULL;                  // the JWK Set itself (a URL or a file) - instead of discovery
static char*  issuer        = NULL;                  // the iss a token must have - NULL: the discovery document's issuer, else not checked
static char*  audience      = NULL;                  // an aud a token must have - NULL: not checked (APISIX does not check it)
static int    leeway        = 120;                   // seconds of clock skew on exp / nbf - lua-resty-openidc's slack, what APISIX allows
static char*  pap           = NULL;                  // the ODRL-PAP (its base URL) - the policies
static char*  policies      = NULL;                  // the policies themselves (a URL or a file: a JSON array of ODRL policies) - instead of the PAP
static char*  organization  = NULL;                  // the provider's DID - the PAP's GENERAL_ORGANIZATION_DID - vc:role looks for its roles
static int    refresh       = 30;                    // seconds between two fetches of keys and policies
static bool   strippedPath  = false;                 // the x-stripped-path header is a gateway's, not a client's: http:path reads it
static bool   anonymousOk   = false;                 // a request without a token is let in, as no one - only a policy for vc:any can allow it anything

static CorArg fdscArgs[] =
{
  { "--fdscDiscovery", "-fdscDiscovery", CorArgString, _vp &discovery,   CorArgOpt, _vp NULL,  NULL,     NULL,          "the verifier's OpenID discovery document - the URL APISIX's openid-connect has as 'discovery' (http://verifier:3000/services/<service>/.well-known/openid-configuration)" },
  { "--fdscJwks",      "-fdscJwks",      CorArgString, _vp &jwks,        CorArgOpt, _vp NULL,  NULL,     NULL,          "the verifier's JWK Set - a URL or a file - instead of --fdscDiscovery" },
  { "--fdscIssuer",    "-fdscIssuer",    CorArgString, _vp &issuer,      CorArgOpt, _vp NULL,  NULL,     NULL,          "the issuer (iss) a token must have - default: the discovery document's" },
  { "--fdscAudience",  "-fdscAudience",  CorArgString, _vp &audience,    CorArgOpt, _vp NULL,  NULL,     NULL,          "an audience (aud) a token must have - the verifier's service id; default: not checked" },
  { "--fdscLeeway",    "-fdscLeeway",    CorArgInt,    _vp &leeway,      CorArgOpt, _vp 120,   _vp 0,    _vp 600,       "seconds of clock skew allowed on a token's exp and nbf - 120, as APISIX's openid-connect allows" },
  { "--fdscPap",       "-fdscPap",       CorArgString, _vp &pap,         CorArgOpt, _vp NULL,  NULL,     NULL,          "the ODRL-PAP's base URL (http://odrl-pap:8080) - the policies are read from its /policy" },
  { "--fdscPolicies",  "-fdscPolicies",  CorArgString, _vp &policies,    CorArgOpt, _vp NULL,  NULL,     NULL,          "the ODRL policies - a URL or a file, a JSON array - instead of --fdscPap" },
  { "--fdscOrganization", "-fdscOrganization", CorArgString, _vp &organization, CorArgOpt, _vp NULL, NULL, NULL,       "the provider's DID - the PAP's GENERAL_ORGANIZATION_DID: vc:role reads the credential's roles for it" },
  { "--fdscRefresh",   "-fdscRefresh",   CorArgInt,    _vp &refresh,     CorArgOpt, _vp 30,    _vp 1,    _vp 86400,     "seconds between two fetches of the keys and the policies" },
  { "--fdscAnonymous", "-fdscAnonymous", CorArgBool,   _vp &anonymousOk, CorArgOpt, _vp false, _vp false, _vp true,     "a request without a token is not a 401 - it is let in as no one, and only a policy for vc:any can allow it" },
  { "--fdscStrippedPath", "-fdscStrippedPath", CorArgBool, _vp &strippedPath, CorArgOpt, _vp false, _vp false, _vp true, "http:path reads the x-stripped-path header, as the connector's Rego does - only behind a gateway that sets it and drops a client's; default: the request's path" },
  CORARGS_END
};



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
static char*            jwksUrl        = NULL;      // from the discovery document - malloc'd
static char*            discoveredIss  = NULL;      // ditto



// -----------------------------------------------------------------------------
//
// discoveryRead - the discovery document's jwks_uri and issuer
//
static bool discoveryRead(char* errorBuf, int errorBufSize)
{
  CorAlloc ka;
  char     buf[4096];

  corAllocBufferInit(&ka, buf, sizeof(buf), 8192, NULL, "fdsc discovery");

  char* text = fdscFetch(discovery, &ka, errorBuf, errorBufSize);
  bool  ok   = false;

  if (text != NULL)
  {
    CorJson cj;
    corJsonCreate(&cj, &ka);

    CorNode* rootP = corJsonParse(&cj, text);
    CorNode* uriP  = (rootP != NULL) ? corTreeLookup(rootP, "jwks_uri") : NULL;
    CorNode* issP  = (rootP != NULL) ? corTreeLookup(rootP, "issuer")   : NULL;

    if ((uriP != NULL) && (uriP->type == CorString))
    {
      pthread_mutex_lock(&refreshMutex);
      free(jwksUrl);
      jwksUrl = strdup(uriP->value.s);

      if ((issP != NULL) && (issP->type == CorString) && ((discoveredIss == NULL) || (strcmp(discoveredIss, issP->value.s) != 0)))
      {
        free(discoveredIss);
        discoveredIss = strdup(issP->value.s);
      }
      pthread_mutex_unlock(&refreshMutex);

      ok = true;
    }
    else
      snprintf(errorBuf, errorBufSize, "%s: no jwks_uri", discovery);
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
    COR_W("fdsc: the verifier's discovery document: %s", errorBuf);
    return;
  }

  CorAlloc    ka;
  char        buf[8192];
  const char* where = (jwks != NULL) ? jwks : jwksUrl;

  corAllocBufferInit(&ka, buf, sizeof(buf), 16384, NULL, "fdsc keys");

  char* text = fdscFetch(where, &ka, errorBuf, sizeof(errorBuf));

  if ((text == NULL) || (fdscKeysLoad(text, errorBuf, sizeof(errorBuf)) == false))
    COR_W("fdsc: the verifier's keys (%s): %s", where, errorBuf);
  else
    __atomic_store_n(&keysFetchedAt, time(NULL), __ATOMIC_RELAXED);

  corAllocBufferReset(&ka, false);
}



// -----------------------------------------------------------------------------
//
// policiesFetch -
//
//
// The PAP's GET /policy is paged (25 by default) and says nothing of how many there are: pages until one
// is empty. The set is installed only once every page is read - a fetch that fails half-way keeps the old.
//
static void policiesFetch(void)
{
  char errorBuf[512] = "";
  char where[1024];
  int  total = 0;

  fdscPolicySetBegin();

  for (int page = 0; page < 10000; page++)
  {
    if (policies != NULL)
      snprintf(where, sizeof(where), "%s", policies);
    else
      snprintf(where, sizeof(where), "%s/policy?page=%d&pageSize=100", pap, page);

    CorAlloc ka;
    char     buf[8192];

    corAllocBufferInit(&ka, buf, sizeof(buf), 64 * 1024, NULL, "fdsc policies");

    int   items = 0;
    char* text  = fdscFetch(where, &ka, errorBuf, sizeof(errorBuf));
    int   n     = (text != NULL) ? fdscPolicySetAdd(text, &items, errorBuf, sizeof(errorBuf)) : -1;

    corAllocBufferReset(&ka, false);

    if (n < 0)
    {
      COR_W("fdsc: the policies (%s): %s", where, errorBuf);
      return;                                        // the set begun is dropped by the next Begin
    }

    total += n;

    if ((policies != NULL) || (items == 0))
      break;
  }

  fdscPolicySetCommit();

  static int lastTotal = -1;

  if (total != lastTotal)
    COR_I("fdsc: %d policies in use", total);
  lastTotal = total;
}



// -----------------------------------------------------------------------------
//
// refreshLoop - keys and policies now, and every --fdscRefresh seconds; keys sooner when a token names one
// the set lacks (the verifier generates its key at every start), but not more than once a second - a client
// sending made-up kids does not make the broker hammer the verifier
//
static void* refreshLoop(void* arg)
{
  (void) arg;

  while (true)
  {
    keysFetch();
    policiesFetch();

    pthread_mutex_lock(&refreshMutex);

    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);

    //
    // Until both are there, every second: the verifier and the PAP may start after the broker
    //
    until.tv_sec += ((fdscKeysCount() == 0) || (fdscPolicyLoaded() == false)) ? 1 : refresh;

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
// fdscInit -
//
static bool fdscInit(char* errorBuf, int errorBufSize)
{
  if ((discovery == NULL) && (jwks == NULL))
  {
    snprintf(errorBuf, errorBufSize, "no keys to check a token with: --fdscDiscovery or --fdscJwks");
    return false;
  }

  if ((jwks != NULL) && (issuer == NULL))
  {
    snprintf(errorBuf, errorBufSize, "--fdscJwks needs --fdscIssuer: a token's iss is always checked");
    return false;
  }

  if ((pap == NULL) && (policies == NULL))
  {
    snprintf(errorBuf, errorBufSize, "no policies to decide with: --fdscPap or --fdscPolicies");
    return false;
  }

  if (pthread_create(&refreshThread, NULL, refreshLoop, NULL) != 0)
  {
    snprintf(errorBuf, errorBufSize, "cannot start the refresh thread");
    return false;
  }

  fdscPolicyOrganizationSet(organization);
  fdscPolicyStrippedPathSet(strippedPath);
  refreshRunning = true;
  COR_I("fdsc: keys from %s, policies from %s - every %d s", (jwks != NULL) ? jwks : discovery, (policies != NULL) ? policies : pap, refresh);
  return true;
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
// fdscAuthenticate -
//
static AuthVerdict fdscAuthenticate(const AuthRequest* reqP, void** identityP, AuthAnswer* answerP)
{
  const char* token = bearer(reqP);

  if (token == NULL)
  {
    if (anonymousOk == true)
      return AuthAllow;                              // identity NULL: no one

    answerP->detail = "no access token (Authorization: Bearer)";
    return AuthUnauthenticated;
  }

  if (fdscKeysCount() == 0)
  {
    answerP->detail = "the verifier's keys are not loaded yet";
    return AuthUnavailable;
  }

  FdscIdentity* idP = fdscIdentityNew();

  if (idP == NULL)
  {
    answerP->detail = "out of memory";
    return AuthUnavailable;
  }

  FdscJwtExpect expect;

  //
  // The issuer - copied while the lock is held: the refresh thread replaces the discovered one
  //
  pthread_mutex_lock(&refreshMutex);
  const char* iss = (issuer != NULL) ? issuer : discoveredIss;
  expect.issuer = (iss != NULL) ? corAllocStrdup(&idP->ka, iss) : NULL;
  pthread_mutex_unlock(&refreshMutex);

  if (expect.issuer == NULL)                         // a token of no known issuer is no token: never accepted unchecked
  {
    fdscIdentityFree(idP);
    answerP->detail = "the verifier's issuer is not known (its discovery document has none, and --fdscIssuer is not set)";
    return AuthUnavailable;
  }

  expect.audience = audience;
  expect.now      = time(NULL);
  expect.leeway   = leeway;

  CorNode*     claimsP = NULL;
  FdscJwtCheck check   = fdscJwtVerify(token, &expect, &idP->ka, &claimsP);

  if (check != FdscJwtOk)
  {
    if (check == FdscJwtUnknownKey)
      refreshAsk();

    fdscIdentityFree(idP);

    static __thread char wwwAuth[160];
    snprintf(wwwAuth, sizeof(wwwAuth), "Bearer error=\"invalid_token\", error_description=\"%s\"", fdscJwtCheckString(check));

    answerP->detail          = fdscJwtCheckString(check);
    answerP->wwwAuthenticate = wwwAuth;
    return AuthUnauthenticated;
  }

  fdscIdentityFill(idP, claimsP);
  *identityP = idP;
  return AuthAllow;
}



// -----------------------------------------------------------------------------
//
// fdscAuthorize -
//
static AuthVerdict fdscAuthorize(const AuthRequest* reqP, void* identity, AuthAnswer* answerP)
{
  if (fdscPolicyLoaded() == false)
  {
    answerP->detail = "the policies are not loaded yet";
    return AuthUnavailable;
  }

  return fdscPolicyDecide(reqP, (FdscIdentity*) identity, answerP);
}



// -----------------------------------------------------------------------------
//
// fdscRelease -
//
static void fdscRelease(void* identity)
{
  fdscIdentityFree((FdscIdentity*) identity);
}



// -----------------------------------------------------------------------------
//
// fdscVersionInfo -
//
static const char* fdscVersionInfo(void)
{
  static __thread char info[160];

  snprintf(info, sizeof(info), "fdsc %s - %d keys, %d policies", FDSC_VERSION, fdscKeysCount(), fdscPolicyCount());
  return info;
}



// -----------------------------------------------------------------------------
//
// fdscClose -
//
static void fdscClose(void)
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

  fdscKeysRelease();
  fdscPolicyRelease();
  free(jwksUrl);
  free(discoveredIss);
  jwksUrl       = NULL;
  discoveredIss = NULL;
}



// -----------------------------------------------------------------------------
//
// authRegister - the plugin's entry point
//
void authRegister(AuthDriver* driverP)
{
  driverP->abiVersion   = AUTH_ABI_VERSION;
  driverP->name         = "fdsc";
  driverP->version      = FDSC_VERSION;
  driverP->args         = fdscArgs;
  driverP->init         = fdscInit;
  driverP->authenticate = fdscAuthenticate;
  driverP->authorize    = fdscAuthorize;
  driverP->release      = fdscRelease;
  driverP->versionInfo  = fdscVersionInfo;
  driverP->close        = fdscClose;
}
