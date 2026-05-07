//
// FILE            postJsonldContexts.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/jsonldContexts — Add JSON-LD Context.
// NGSI-LD v1.9.1 § 5.13.2.
//
// Two body forms (§ 5.13.2.2):
//   Hosted:  { "@context": { ... } }              → stored verbatim
//   Cached:  { "url": "http://example/ctx.jsonld" } → broker downloads
//
// On success: 201 Created + Location: /ngsi-ld/v1/jsonldContexts/{contextId}.
// For Cached, the context id equals the downloaded URL. For Hosted, the
// broker mints a fresh urn:ngsi-ld:Context: id.
//

#include <stddef.h>                                   // NULL
#include <string.h>                                   // strlen, strcpy, strcat

#include "swRest/SwRestState.h"                       // swRest
#include "swRest/swRestOutHeader.h"                   // swRestOutHeaderAdd
#include "kalloc/kaAlloc.h"                           // kaAlloc
#include "kalloc/kaStrdup.h"                          // kaStrdup
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjRenderSize.h"                       // kjFastRenderSize
#include "kjson/kjRender.h"                           // kjFastRender
#include "swJsonld/SwldContext.h"                     // SwldContext, SwldContextKind
#include "swJsonld/SwldContextCache.h"                // SwldContextCache
#include "swJsonld/swldCache.h"                       // swldCacheLookup, swldCacheInsert
#include "swJsonld/swldContextParse.h"                // swldContextFromObject, swldContextFromTree
#include "swJsonld/swldDownload.h"                    // swldContextFromUrl
#include "swJsonld/swldIdGen.h"                       // swldIdGenerate
#include "swNgsild/swNgsild.h"                        // ldError, LD_ERROR_*, swNgsild

#include "db/DbDriver.h"                              // db, DB_CONTEXT_KIND_*

#include "serviceRoutines/postJsonldContexts.h"       // Own interface



// -----------------------------------------------------------------------------
//
// swldCacheGet - internal accessor in swJsonld/swldInit.c
//
extern SwldContextCache* swldCacheGet(void);



// -----------------------------------------------------------------------------
//
// postJsonldContexts -
//
bool postJsonldContexts(void)
{
  KjNode* bodyP = swRest.in.requestTree;

  //
  // Content-Type / payload checks
  //
  if (bodyP->type != KjObject)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "payload must be a JSON object");
    return true;
  }

  //
  // Dispatch on body shape: @context ⇒ Hosted, url ⇒ Cached.
  //
  KjNode* atContextP = kjLookup(bodyP, "@context");
  KjNode* urlP       = kjLookup(bodyP, "url");

  SwldContextCache* cacheP   = swldCacheGet();
  KAlloc*           storeP   = cacheP->kaP;
  const char*       location = NULL;

  if (atContextP != NULL)
  {
    //
    // HOSTED — body is a full JSON-LD Context document. Store it verbatim
    // and parse the inner @context into the name/value hash tables.
    //
    if (atContextP->type != KjObject && atContextP->type != KjArray)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Context",
              "'@context' must be a JSON object or array");
      return true;
    }

    SwldContext* contextP = NULL;

    if (atContextP->type == KjObject)
      contextP = swldContextFromObject(atContextP, storeP, NULL);
    else
      //
      // Array form. Each element is a URL string (downloaded as Implicit
      // and referenced) or an inline object. swldContextFromTree builds
      // a wrapper SwldContext with isArray=true and contextV[] populated.
      //
      contextP = swldContextFromTree(atContextP, storeP);

    if (contextP == NULL)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Context",
              "unable to parse JSON-LD context");
      return true;
    }

    //
    // § 5.13.2.5 (ETSI 050_04): "Implementations may also create new
    // @context entries through references to other @contexts (using
    // @context arrays). When a referenced @context is not yet stored,
    // the broker downloads it and adds it as a Cached @context."
    //
    // swldContextFromTree just-downloaded each URL element of the
    // @context array as kind=Implicit; promote those to Cached so a
    // subsequent GET ?details=true reports the spec-mandated kind.
    // Inline-object elements stay Implicit (they have no URL — they
    // can't be Cached per definition).
    //
    if (atContextP->type == KjArray)
    {
      for (KjNode* el = atContextP->value.firstChildP; el != NULL; el = el->next)
      {
        if (el->type != KjString) continue;
        SwldContext* refP = swldCacheLookup(el->value.s);
        if (refP != NULL && refP->kind == SwldKindImplicit && refP->url != NULL)
        {
          refP->kind = SwldKindCached;
          if (db.contextSave != NULL && refP->body != NULL)
            db.contextSave(refP->url, refP->url, DB_CONTEXT_KIND_CACHED, refP->body);
        }
      }
    }

    //
    // Assign broker-minted id and preserve the raw body for later GETs.
    //
    char* id = swldIdGenerate(storeP);
    if (id == NULL)
    {
      ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "id generation failed");
      return true;
    }
    contextP->id   = id;
    contextP->kind = SwldKindHosted;

    int   bodyLen = kjFastRenderSize(bodyP) + 1;
    char* bodyBuf = (char*) kaAlloc(storeP, bodyLen);
    if (bodyBuf != NULL)
    {
      kjFastRender(bodyP, bodyBuf);
      contextP->body = bodyBuf;
    }

    swldCacheInsert(contextP);

    //
    // Persist (only mongoc plugin implements this; ramdb leaves it NULL).
    //
    if (db.contextSave != NULL && contextP->body != NULL)
      db.contextSave(id, NULL, DB_CONTEXT_KIND_HOSTED, contextP->body);

    location = id;
  }
  else if (urlP != NULL)
  {
    //
    // CACHED — broker downloads the URL and caches.
    //
    if (urlP->type != KjString || urlP->value.s[0] == '\0')
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "'url' must be a non-empty string");
      return true;
    }

    const char* url = urlP->value.s;

    //
    // If already in cache (as Implicit): upgrade to Cached per § 5.13.2.5.
    // Otherwise download. Either way, location = url.
    //
    SwldContext* existingP = swldCacheLookup(url);

    if (existingP == NULL)
    {
      existingP = swldContextFromUrl(url, storeP);

      if (existingP == NULL)
      {
        ldError(504, LD_ERROR_LD_CONTEXT_NOT_AVAILABLE, "Context Not Available",
                "unable to retrieve @context from '%s'", url);
        return true;
      }
    }

    // Upgrade kind to Cached (idempotent if already Cached/Hosted).
    if (existingP->kind == SwldKindImplicit)
      existingP->kind = SwldKindCached;

    //
    // Persist. body was captured at download time (or by an earlier POST).
    //
    if (db.contextSave != NULL && existingP->body != NULL)
      db.contextSave(url, url, DB_CONTEXT_KIND_CACHED, existingP->body);

    location = url;
  }
  else
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "payload must contain either '@context' (Hosted) or 'url' (Cached)");
    return true;
  }

  //
  // 201 Created + Location
  //
  swRest.out.httpStatusCode = 201;

  //
  // Build "/ngsi-ld/v1/jsonldContexts/<id>" in the request arena.
  //
  static const char prefix[] = "/ngsi-ld/v1/jsonldContexts/";
  int   locLen = sizeof(prefix) - 1 + strlen(location) + 1;
  char* locBuf = (char*) kaAlloc(&swRest.kalloc, locLen);
  if (locBuf != NULL)
  {
    strcpy(locBuf, prefix);
    strcat(locBuf, location);

    swRestOutHeaderAdd("Location", locBuf);
  }

  return true;
}
