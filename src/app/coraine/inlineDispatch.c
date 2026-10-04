//
// FILE            inlineDispatch.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#define _GNU_SOURCE                                    // memmem

#include <stdbool.h>                                   // bool
#include <string.h>                                    // strcmp, strncmp, strcasecmp, strstr, strchr, strrchr, strlen, memcpy, memchr, memmem

#include "corLog/corLog.h"                             // COR_V
#include "corTree/CorNode.h"                           // CorNode, CorString, CorObject, CorArray
#include "corRest/CorRestState.h"                      // corRest
#include "corRest/corRestHooks.h"                      // corRestSetInlineHook, corRestSetFinishInlineHook, corRestSetFinishCoroutineHook
#include "corJsonld/corLdCache.h"                      // corLdCacheLookup
#include "corNgsild/CorNgsild.h"                       // ldDefaultContextUrl, ldDistributed, corNgsild
#include "corNgsild/ldRegCache.h"                      // ldRegCacheItemsTotal

#include "bridge/bridgeServiceSync.h"                  // bridgeSyncDefault

#include "inlineDispatch.h"                            // Own interface



// -----------------------------------------------------------------------------
//
// ENTITIES - the path family that may run inline: /ngsi-ld/v1/entities and everything below it
//
static const char ENTITIES[]  = "/ngsi-ld/v1/entities";
static const int  ENTITIES_LEN = sizeof(ENTITIES) - 1;



// -----------------------------------------------------------------------------
//
// urlCached - is this @context URL (the len bytes at urlP, not NUL-terminated) in the cache?
//
static bool urlCached(const char* urlP, int len)
{
  char url[1024];

  if (len >= (int) sizeof(url))
    return false;

  memcpy(url, urlP, len);
  url[len] = 0;

  return corLdCacheLookup(url) != NULL;
}



// -----------------------------------------------------------------------------
//
// Raw-body scanning - just enough JSON to find a body's @context without parsing the body
//
// Each returns the position right after what it skipped, or NULL for anything it does not follow -
// and NULL always means "hand it off": the scan may be too cautious, never too bold.
//
static const char* ws(const char* p)
{
  while ((*p == ' ') || (*p == '\t') || (*p == '\n') || (*p == '\r'))
    ++p;
  return p;
}

static const char* stringEnd(const char* p)            // p at the opening quote
{
  for (++p; *p != 0; ++p)
  {
    if (*p == '\\')
    {
      if (p[1] == 0)
        return NULL;
      ++p;
    }
    else if (*p == '"')
      return p + 1;
  }
  return NULL;
}

static const char* valueEnd(const char* p)             // p at the first character of a value
{
  if (*p == '"')
    return stringEnd(p);

  if ((*p != '{') && (*p != '['))
  {
    while ((*p != 0) && (*p != ',') && (*p != '}') && (*p != ']'))
      ++p;
    return p;
  }

  int depth = 0;
  for (; *p != 0; ++p)
  {
    if (*p == '"')
    {
      if ((p = stringEnd(p)) == NULL)
        return NULL;
      --p;
    }
    else if ((*p == '{') || (*p == '['))
      ++depth;
    else if (((*p == '}') || (*p == ']')) && (--depth == 0))
      return p + 1;
  }
  return NULL;
}



// -----------------------------------------------------------------------------
//
// contextValueCached - does this @context value need nothing that is not cached?
//
// A string is one URL; an array is URLs and inline objects; an object is an inline context - with
// no I/O in it unless it imports one (@import) or holds a scoped context of its own (@context).
//
static bool contextValueCached(const char* p, const char* end)
{
  if (*p == '"')
  {
    const char* close = stringEnd(p);
    if ((close == NULL) || (memchr(p, '\\', close - p) != NULL))
      return false;                                      // an escaped URL - the parser decides
    return urlCached(p + 1, (int) (close - p - 2));
  }

  if (*p == '{')
  {
    int len = (int) (end - p);
    return (memmem(p + 1, len - 1, "\"@import\"", 9) == NULL) && (memmem(p + 1, len - 1, "\"@context\"", 10) == NULL);
  }

  if (*p == '[')
  {
    for (p = ws(p + 1); (*p != 0) && (*p != ']'); p = ws(p))
    {
      const char* itemEnd = valueEnd(p);

      if ((itemEnd == NULL) || (contextValueCached(p, itemEnd) == false))
        return false;

      p = ws(itemEnd);
      if (*p == ',')
        ++p;
    }
    return *p == ']';
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// Tree-body checks - a cor:// request carries its body already parsed (corRest.in.requestTree), with
// no text to scan; the same answers as the raw scan, from the tree
//
static bool treeImports(CorNode* nodeP)                // any @import or scoped @context, at any depth?
{
  for (CorNode* childP = nodeP->value.head; childP != NULL; childP = childP->next)
  {
    if ((nodeP->type == CorObject) && ((strcmp(childP->name, "@import") == 0) || (strcmp(childP->name, "@context") == 0)))
      return true;

    if (((childP->type == CorObject) || (childP->type == CorArray)) && (treeImports(childP) == true))
      return true;
  }
  return false;
}

static bool treeContextValueCached(CorNode* nodeP)
{
  if (nodeP->type == CorString)
    return corLdCacheLookup(nodeP->value.s) != NULL;

  if (nodeP->type == CorObject)
    return treeImports(nodeP) == false;

  if (nodeP->type == CorArray)
  {
    for (CorNode* itemP = nodeP->value.head; itemP != NULL; itemP = itemP->next)
    {
      if (treeContextValueCached(itemP) == false)
        return false;
    }
    return true;
  }

  return false;
}

static bool treeBodyContextCached(CorNode* bodyP)
{
  if (bodyP->type != CorObject)
    return false;

  for (CorNode* memberP = bodyP->value.head; memberP != NULL; memberP = memberP->next)
  {
    if (strcmp(memberP->name, "@context") == 0)
      return treeContextValueCached(memberP);
  }

  return true;                                           // no @context: a 400, no download
}



// -----------------------------------------------------------------------------
//
// bodyContextCached - an application/ld+json body: is the @context it carries cached?
//
// Walks the top-level members of the (object) body to "@context". An array body is a batch - not
// of this family - and no body at all or no @context is left to the ordinary path, which answers
// 400 without any I/O.
//
static bool bodyContextCached(void)
{
  const char* p = corRest.in.payload;

  if ((p == NULL) && (corRest.in.requestTree != NULL))
    return treeBodyContextCached(corRest.in.requestTree);

  if ((p == NULL) || (*(p = ws(p)) != '{'))
    return (p == NULL) || (*p == 0);

  for (p = ws(p + 1); *p == '"'; )
  {
    const char* keyEnd = stringEnd(p);
    if (keyEnd == NULL)
      return false;

    bool isContext = ((keyEnd - p) == 10) && (strncmp(p, "\"@context\"", 10) == 0);

    p = ws(keyEnd);
    if (*p != ':')
      return false;
    p = ws(p + 1);

    const char* end = valueEnd(p);
    if (end == NULL)
      return false;

    if (isContext == true)
      return contextValueCached(p, end);

    p = ws(end);
    if (*p == ',')
      p = ws(p + 1);
  }

  return true;                                           // no @context: a 400, no download
}



// -----------------------------------------------------------------------------
//
// contextCached - is the @context this request will expand with already in the cache?
//
// The Link header's URL, else the default user context; none at all = the core context, which is
// always there. A URL too long for the buffer is answered "no" - handed off, never wrong.
//
static bool contextCached(void)
{
  const char* urlP = NULL;
  char        url[1024];

  for (int ix = 0; ix < corRest.in.httpHeaderCount; ix++)
  {
    const char* v = corRest.in.httpHeaderV[ix].value;

    if ((strcasecmp(corRest.in.httpHeaderV[ix].key, "Link") != 0) || (strstr(v, "json-ld#context") == NULL))
      continue;

    const char* end = (v[0] == '<') ? strchr(v + 1, '>') : NULL;

    if ((end == NULL) || (end - (v + 1) >= (long) sizeof(url)))
      return false;                                      // one the ordinary path will make sense of, or reject

    memcpy(url, v + 1, end - (v + 1));
    url[end - (v + 1)] = 0;
    urlP = url;
    break;
  }

  if (urlP == NULL)
    urlP = ldDefaultContextUrl;

  return (urlP == NULL) || (corLdCacheLookup(urlP) != NULL);
}



// -----------------------------------------------------------------------------
//
// ddsSyncWaits - will this request wait for a bridge service's reply? (?ddsSync, --ddsSync)
//
// A PATCH of a service attribute with ddsSync waits up to --ddsSyncTimeout for the service to answer
// (bridgeServiceSync, bridgeGoalAwait). ?ddsSync=true / =false decides; without it, --ddsSync does.
// The backend has split the query into uriParamV by now (the raw urlParams is gone).
//
static bool ddsSyncWaits(void)
{
  for (int ix = 0; ix < corRest.in.uriParamCount; ix++)
  {
    if (strcmp(corRest.in.uriParamV[ix].key, "ddsSync") == 0)
      return (corRest.in.uriParamV[ix].value != NULL) && (strcmp(corRest.in.uriParamV[ix].value, "true") == 0);
  }

  return bridgeSyncDefault;
}



// -----------------------------------------------------------------------------
//
// inlineDispatchCheck - may this request run on the I/O thread that read it? (CorRestInlineHook)
//
static bool inlineDispatchCheck(void)
{
  const char* path = corRest.in.urlPath;

  if ((path == NULL) || (strncmp(path, ENTITIES, ENTITIES_LEN) != 0))
    return false;

  if ((path[ENTITIES_LEN] != 0) && (path[ENTITIES_LEN] != '/'))
    return false;                                        // /ngsi-ld/v1/entitiesX is not the family

  if ((ldDistributed == true) && (ldRegCacheItemsTotal() != 0))
    return false;                                        // it could become a distributed operation

  if (ddsSyncWaits() == true)
    return false;                                        // it waits for a bridge service's reply

  if (corRest.in.contentMime == CorMimeLdJson)
    return bodyContextCached();                          // the body's own @context - and no Link

  return contextCached();
}



// -----------------------------------------------------------------------------
//
// finishInlineCheck - may this request's post-response phase run on the I/O thread? (CorRestFinishInlineHook)
//
// The phase is the deferred work of the request (brokerPostResponseHook). Only some of it can wait
// on something - a notification (an @context the broker may host itself), a CSR notification, a
// bridge goal released, an expired entity deleted, a registration probed - and it is all recorded,
// per request, in corNgsild. Nothing recorded: nothing to wait for, so no worker. TRoE events stay
// in-process (the hook is only installed with TRoE none or corDB).
//
static bool finishInlineCheck(void)
{
  return (corNgsild.pendingN       == 0)    &&
         (corNgsild.csrPendingN    == 0)    &&
         (corNgsild.bridgeReleaseQ == NULL) &&
         (corNgsild.expiredN       == 0)    &&
         (corNgsild.probePendingN  == 0);
}



// -----------------------------------------------------------------------------
//
// coroutineCheck - may a request that can wait run as a coroutine of its loop? (CorRestCoroutineHook)
//
// Only where every wait it can meet yields - corRest's clients and the @context download do. Not with
// a database (or TRoE) plugin that blocks inside its driver (mongoc, timescale), nor for a PATCH that
// waits for a bridge service's reply (?ddsSync - a condition variable): either would stop the whole
// loop for its wait. Those keep the worker (doc/coroutines.md § 1).
//
static bool coroutineDbOk = false;                       // corDB, with TRoE none or corDB - set at start-up

static bool coroutineCheck(void)
{
  return (coroutineDbOk == true) && (ddsSyncWaits() == false);
}



// -----------------------------------------------------------------------------
//
// finishCoroutineCheck - may a post-response phase that can wait run as a coroutine? (CorRestFinishCoroutineHook)
//
// What it can wait for: notifications - HTTP through corRest's client, which yields; any other scheme
// (mqtt://) through a bridge plugin's own client, which corNgsild runs on a thread of its own
// (corCoBlocking) - CSR notifications, the registration probe (HTTP both), the expired entities and
// TRoE (in-process with corDB). Not a bridge goal's release, which waits on the bridge: a worker.
//
static bool finishCoroutineCheck(void)
{
  return (coroutineDbOk == true) && (corNgsild.bridgeReleaseQ == NULL);
}



// -----------------------------------------------------------------------------
//
// isPlugin - is this plugin argument (a short name, or a path to the .so) the plugin 'name'?
//
static bool isPlugin(const char* arg, const char* name)
{
  const char* slash = strrchr(arg, '/');
  const char* base  = (slash != NULL) ? slash + 1 : arg;
  int         len   = strlen(name);

  return (strncmp(base, name, len) == 0) && ((base[len] == 0) || (strcmp(&base[len], ".so") == 0));
}



// -----------------------------------------------------------------------------
//
// neverWaits - a store in the process that never waits on I/O: corDB, or ramDB (corDB in RAM only) -
// the condition for inline dispatch and coroutines, asked of what the store IS, not of one name
//
static bool neverWaits(const char* dbName, const char* troeName)
{
  bool db   = isPlugin(dbName, "corDB") || isPlugin(dbName, "ramDB");
  bool troe = isPlugin(troeName, "none") || isPlugin(troeName, "corDB");

  return db && troe;
}



// -----------------------------------------------------------------------------
//
// inlineDispatchInit -
//
void inlineDispatchInit(const char* dbName, const char* troeName, bool disabled)
{
  //
  // Coroutines: their own question, and asked whatever the database - a mongoc broker answers 'no'
  //
  coroutineDbOk = neverWaits(dbName, troeName);
  corRestSetCoroutineHook(coroutineCheck);
  corRestSetFinishCoroutineHook(finishCoroutineCheck);

  if (disabled == true)
  {
    COR_V("inline dispatch: off (--noInline) - every request is handed to a worker");
    return;
  }

  if (neverWaits(dbName, troeName) == false)
  {
    COR_V("inline dispatch: off - database '%s', TRoE '%s' (only corDB or ramDB, with TRoE none/corDB, never waits)", dbName, troeName);
    return;
  }

  corRestSetInlineHook(inlineDispatchCheck);
  corRestSetFinishInlineHook(finishInlineCheck);         // asked by the built-in server only
  COR_V("inline dispatch: on");
}
