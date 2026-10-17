//
// FILE            startupDefaultUserContext.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdio.h>                                        // fopen, fread, snprintf
#include <stdlib.h>                                       // malloc, free
#include <string.h>                                       // strstr, strncmp, strlen
#include <time.h>                                         // time

#include "corAlloc/CorAlloc.h"                            // CorAlloc
#include "corAlloc/corAlloc.h"                            // corAlloc
#include "corAlloc/corAllocStrdup.h"                      // corAllocStrdup
#include "corJson/CorJson.h"                              // CorJson
#include "corJson/corJsonCreate.h"                        // corJsonCreate
#include "corJson/corJsonParse.h"                         // corJsonParse
#include "corTree/CorNode.h"                              // CorNode
#include "corTree/corTreeLookup.h"                        // corTreeLookup
#include "corJsonld/CorLdContext.h"                       // CorLdContext, CorLdKindHosted
#include "corJsonld/CorLdContextCache.h"                  // CorLdContextCache
#include "corJsonld/corLdCache.h"                         // corLdCacheInsert
#include "corJsonld/corLdContextParse.h"                  // corLdContextFromObject, corLdContextFromTree

#include "startup/startupDefaultUserContext.h"            // Own interface

extern CorLdContextCache* corLdCacheGet(void);            // corJsonld/corLdInit.c - the cache's allocator



// -----------------------------------------------------------------------------
//
// fileRead - a whole file, NUL-terminated (malloc); NULL: not readable
//
static char* fileRead(const char* path)
{
  FILE* fP = fopen(path, "r");

  if (fP == NULL)
    return NULL;

  char* text = NULL;
  long  size = (fseek(fP, 0, SEEK_END) == 0) ? ftell(fP) : -1;

  if ((size >= 0) && (fseek(fP, 0, SEEK_SET) == 0) && ((text = (char*) malloc(size + 1)) != NULL))
  {
    if (fread(text, 1, size, fP) == (size_t) size)
      text[size] = 0;
    else
    {
      free(text);
      text = NULL;
    }
  }

  fclose(fP);
  return text;
}



// -----------------------------------------------------------------------------
//
// startupDefaultUserContext -
//
const char* startupDefaultUserContext(const char* duc, const char* endpoint, const char** errorP)
{
  static char error[512];
  static char url[512];

  *errorP = NULL;

  if (duc == NULL)
    return NULL;

  //
  // A URL other than file:// - downloaded as any @context, when first needed
  //
  bool fileUrl = (strncmp(duc, "file://", 7) == 0);

  if ((fileUrl == false) && (strstr(duc, "://") != NULL))
    return duc;

  const char* path = (fileUrl == true) ? &duc[7] : duc;
  char*       text = fileRead(path);

  if (text == NULL)
  {
    snprintf(error, sizeof(error), "--defaultUserContext: the file '%s' cannot be read", path);
    *errorP = error;
    return NULL;
  }

  //
  // Into the @context cache's own allocator - the context lives as long as the broker
  //
  CorAlloc* storeP = corLdCacheGet()->kaP;
  char*     body   = corAllocStrdup(storeP, text);        // the document as the file has it - corJsonParse writes in its buffer
  char*     parsed = corAllocStrdup(storeP, text);
  CorJson   cj;

  free(text);

  corJsonCreate(&cj, storeP);

  CorNode* docP       = ((body != NULL) && (parsed != NULL)) ? corJsonParse(&cj, parsed) : NULL;
  CorNode* atContextP = ((docP != NULL) && (docP->type == CorObject)) ? corTreeLookup(docP, "@context") : NULL;

  if ((atContextP == NULL) || ((atContextP->type != CorObject) && (atContextP->type != CorArray)))
  {
    snprintf(error, sizeof(error), "--defaultUserContext: '%s' is not a JSON-LD @context document - a JSON object with \"@context\" (an object or an array)", path);
    *errorP = error;
    return NULL;
  }

  CorLdContext* contextP = (atContextP->type == CorObject) ? corLdContextFromObject(atContextP, storeP, NULL) : corLdContextFromTree(atContextP, storeP, NULL);

  if (contextP == NULL)
  {
    snprintf(error, sizeof(error), "--defaultUserContext: the @context in '%s' cannot be used", path);
    *errorP = error;
    return NULL;
  }

  snprintf(url, sizeof(url), "%s/ngsi-ld/v1/jsonldContexts/defaultUserContext", endpoint);

  contextP->id        = (char*) "defaultUserContext";
  contextP->url       = corAllocStrdup(storeP, url);
  contextP->body      = body;
  contextP->kind      = CorLdKindHosted;
  contextP->createdAt = (double) time(NULL);
  contextP->usedAt    = contextP->createdAt;

  corLdCacheInsert(contextP);

  return contextP->url;
}
