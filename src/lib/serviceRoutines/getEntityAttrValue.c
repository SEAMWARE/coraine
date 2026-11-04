//
// FILE            getEntityAttrValue.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/entities/{entityId}/attrs/{attrId}/value
//
// Retrieve Attribute Value (TS 104-176 § 7.6.3.1 / TS 104-175 § 10.4.4). The
// response is a "value only" Attribute Fragment: the bare attribute value
// itself — a JSON Primitive, Object or Array — NOT a JSON-LD document. So the
// body carries no @context and the Content-Type is application/json.
//
// Local-only for now (mirrors getEntityAttr); the GET /value distop story
// follows the same path as single-attribute retrieval.
//

#include <stddef.h>                                  // NULL
#include <stdio.h>                                   // snprintf
#include <string.h>                                  // strcmp

#include "corRest/CorRestState.h"                      // corRest

#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeBuilder.h"                  // corTreeObject, corTreeChildAdd, corTreeChildRemove
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corJson/corJsonRender.h"                   // corJsonFastRender
#include "corJson/corJsonRenderSize.h"               // corJsonFastRenderSize

#include "corAlloc/corAlloc.h"                       // corAlloc

#include "corJsonld/corLdExpand.h"                     // corLdExpand
#include "corJsonld/corLdInit.h"                       // corLdCoreContext

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild, ldContextResolve
#include "corNgsild/ldEntityToApi.h"                  // ldEntityToApi
#include "corNgsild/ldStripSysAttrs.h"                // ldStripSysAttrs
#include "corNgsild/ldRender.h"                       // ldAttrValueNode
#include "corNgsild/ldLangReduce.h"                   // ldLangReduce
#include "corJsonld/corLdCompactTree.h"               // corLdCompactTreeWith
#include "corNgsild/ldEntityMatch.h"                  // ldEntityMatchType
#include "corNgsild/ldTermId.h"                          // ldNodeRename

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#if COR_FEATURE_AUTH
#include "auth/auth.h"                               // authEntityStoredCheck
#endif
#include "serviceRoutines/getEntityAttrValue.h"      // Own interface



// -----------------------------------------------------------------------------
//
// getEntityAttrValue -
//
bool getEntityAttrValue(void)
{
  const char* entityId = corRest.in.wildcard[0];
#if COR_FEATURE_AUTH
  if (authEntityStoredCheck(entityId) == false)      // the stored entity's types - before anything is forwarded, bridged or written
    return true;
#endif
  const char* attrWild = corRest.in.wildcard[1];

  ldContextResolve();

  CorLdContext* ctxP    = (corNgsild.contextP != NULL) ? corNgsild.contextP : corLdCoreContext();
  const char*  attrIri = corLdExpand(ctxP, attrWild, &corRest.kalloc, NULL, NULL);
  if (attrIri == NULL)
    attrIri = attrWild;

  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

  CorNode* entityP = NULL;
  int     r       = db.entityRetrieve(tenantP, entityId, &entityP);

  if (r == DB_NOT_FOUND || entityP == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity '%s' not found", entityId);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "database error retrieving entity '%s'", entityId);
    return true;
  }

  // § 10.4.4.4: the entity is found only "where specified type" matches - a ?type that names none of its types
  // is a not-found, like an unknown id
  if ((corNgsild.typeExpr != NULL) && !ldEntityMatchType(corTreeLookup(entityP, "type"), corNgsild.typeExpr))
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity '%s' not found", entityId);
    return true;
  }

  CorNode* attrWrapperP = corTreeLookup(entityP, attrIri);
  if (attrWrapperP == NULL || attrWrapperP->type != CorObject)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "attribute '%s' not found in entity '%s'", attrWild, entityId);
    return true;
  }

  //
  // TS 104-175 § 10.4.4.4: the instances of the datasetIds asked for - and when none is asked for, the default
  // instance only; an Attribute without one is not found. Storage keys each instance by its datasetId, "@none"
  // for the default.
  //
  static char* defaultOnlyV[] = { (char*) "@none", NULL };
  char**       datasetIdV     = (corNgsild.datasetIdV != NULL) ? corNgsild.datasetIdV : defaultOnlyV;
  CorNode*     instP          = attrWrapperP->value.head;

  while (instP != NULL)
  {
    CorNode* nextP = instP->next;
    bool    keep  = false;
    for (int i = 0; datasetIdV[i] != NULL; i++)
    {
      if (instP->name != NULL && strcmp(instP->name, datasetIdV[i]) == 0)
      {
        keep = true;
        break;
      }
    }
    if (!keep)
      corTreeChildRemove(attrWrapperP, instP);
    instP = nextP;
  }

  if (attrWrapperP->value.head == NULL)
  {
    if (corNgsild.datasetIdV != NULL)
      ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
              "no matching datasetId for attribute '%s' in entity '%s'", attrWild, entityId);
    else
      ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
              "attribute '%s' in entity '%s' has no default instance", attrWild, entityId);
    return true;
  }

  //
  // Unwrap storage-format to API-format via a transient single-attribute entity.
  //
  corTreeChildRemove(entityP, attrWrapperP);
  CorNode* wrap = corTreeObject(corRest.kallocP, NULL);
  corTreeChildAdd(wrap, attrWrapperP);

  ldEntityToApi(wrap, &corRest.kalloc);

  // sysAttrs have nowhere to live in a value-only response — always stripped.
  ldStripSysAttrs(wrap);

  //
  // § 10.4.4.3 lists the language filter (§ 7.2.7) among this operation's input: with ?lang a LanguageProperty
  // becomes a Property whose value is the string in that language - so its value-only form is that string, not
  // the languageMap. Same reduction, and at the same point, as GET .../attrs/{attrId}.
  //
  if (corNgsild.lang != NULL)
    ldLangReduce(wrap, corNgsild.lang, &corRest.kalloc);

  //
  // wrap is now { attrName: {type, <valueMember>, ...} } — or, for a
  // multi-instance attribute, { attrName: [ {...}, {...} ] }. value-only returns
  // the instance the datasetId filter above kept (the default one, unless asked otherwise); pick the
  // first instance when an array remains.
  //
  CorNode* attrP = wrap->value.head;
  if (attrP != NULL && attrP->type == CorArray)
    attrP = attrP->value.head;

  CorNode* valueP = (attrP != NULL) ? ldAttrValueNode(attrP) : NULL;
  if (valueP == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "attribute '%s' in entity '%s' has no value to return", attrWild, entityId);
    return true;
  }

  //
  // Render the value alone (no member name, no sibling comma, no @context) —
  // the value-only body. Detach from its siblings so corJsonFastRender emits just
  // the value with no trailing separator.
  //
  ldNodeRename(valueP, NULL);
  valueP->next = NULL;

  //
  // A JSON object value has its member names stored expanded, like any name of the entity. The render hook,
  // which compacts every other response, never sees this one (the body is rendered here, raw: a value-only
  // fragment has no place for an @context), so the value is compacted here, with the @context the request
  // named - the one its attribute name was expanded with, above.
  //
  if ((valueP->type == CorObject) || (valueP->type == CorArray))
    corLdCompactTreeWith(valueP, ctxP, &corRest.kalloc);
  int   len = corJsonFastRenderSize(valueP) + 1;
  char* buf = (char*) corAlloc(&corRest.kalloc, len);
  if (buf == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "out of memory rendering attribute value");
    return true;
  }
  corJsonFastRender(valueP, buf);

  //
  // TS 104-175 § 5.4.2: "An @context cannot be embedded within the Attribute fragment when using the value only
  // representation, therefore in this case, whenever an @context is required, it shall be supplied through the use
  // of a Link header" - whatever the Accept. The render hook writes that header for the responses it renders; this
  // one it does not render.
  //
  if ((ctxP != NULL) && (ctxP->url != NULL))
  {
    static const char suffix[] = ">; rel=\"http://www.w3.org/ns/json-ld#context\"; type=\"application/ld+json\"";
    int               linkLen  = 1 + strlen(ctxP->url) + (sizeof(suffix) - 1) + 1;
    char*             linkBuf  = (char*) corAlloc(&corRest.kalloc, linkLen);

    if (linkBuf != NULL)
    {
      snprintf(linkBuf, linkLen, "<%s%s", ctxP->url, suffix);

      int ix = corRest.out.headerCount;
      corRest.out.headerV[ix].key   = (char*) "Link";
      corRest.out.headerV[ix].value = linkBuf;
      corRest.out.headerCount       = ix + 1;
    }
  }

  corRest.out.payload        = buf;
  corRest.out.payloadSize    = strlen(buf);
  corRest.out.contentType    = (char*) corMimeString(CorMimeJson);
  corRest.out.httpStatusCode = 200;
  corNgsild.rawResponse      = true;
  return true;
}
