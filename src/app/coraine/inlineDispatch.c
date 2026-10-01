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
#include "corRest/CorRestState.h"                      // corRest
#include "corRest/corRestHooks.h"                      // corRestSetInlineHook
#include "corJsonld/corLdCache.h"                      // corLdCacheLookup
#include "corNgsild/CorNgsild.h"                       // ldDefaultContextUrl, ldDistributed
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
// bodyContextCached - an application/ld+json body: is the @context it carries cached?
//
// Walks the top-level members of the (object) body to "@context". An array body is a batch - not
// of this family - and no body at all or no @context is left to the ordinary path, which answers
// 400 without any I/O.
//
static bool bodyContextCached(void)
{
  const char* p = corRest.in.payload;

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
// inlineDispatchInit -
//
void inlineDispatchInit(const char* dbName, const char* troeName, bool disabled)
{
  if (disabled == true)
  {
    COR_V("inline dispatch: off (--noInline) - every request is handed to a worker");
    return;
  }

  if ((isPlugin(dbName, "corDB") == false) || ((isPlugin(troeName, "none") == false) && (isPlugin(troeName, "corDB") == false)))
  {
    COR_V("inline dispatch: off - database '%s', TRoE '%s' (only corDB with TRoE none/corDB never waits)", dbName, troeName);
    return;
  }

  corRestSetInlineHook(inlineDispatchCheck);
  COR_V("inline dispatch: on");
}
