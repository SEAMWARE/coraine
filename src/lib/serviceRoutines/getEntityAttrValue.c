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
#include <string.h>                                  // strcmp

#include "corRest/CorRestState.h"                      // corRest

#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeBuilder.h"                  // corTreeObject, corTreeChildAdd, corTreeChildRemove
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corJson/corJsonRender.h"                   // corJsonFastRender
#include "corJson/corJsonRenderSize.h"               // corJsonFastRenderSize

#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "corJsonld/corLdExpand.h"                     // corLdExpand
#include "corJsonld/corLdInit.h"                       // corLdCoreContext

#include "corNgsild/corNgsild.h"                       // ldError, LD_ERROR_*, corNgsild, ldContextResolve
#include "corNgsild/ldEntityToApi.h"                  // ldEntityToApi
#include "corNgsild/ldStripSysAttrs.h"                // ldStripSysAttrs
#include "corNgsild/ldRender.h"                       // ldAttrValueNode

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getEntityAttrValue.h"      // Own interface



// -----------------------------------------------------------------------------
//
// getEntityAttrValue -
//
bool getEntityAttrValue(void)
{
  const char* entityId = corRest.in.wildcard[0];
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

  CorNode* attrWrapperP = corTreeLookup(entityP, attrIri);
  if (attrWrapperP == NULL || attrWrapperP->type != CorObject)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "attribute '%s' not found in entity '%s'", attrWild, entityId);
    return true;
  }

  //
  // datasetId filter — storage format keys each instance by dsKey (or "@none"
  // for the default). Keep only matching instances.
  //
  if (corNgsild.datasetIdV != NULL)
  {
    CorNode* instP = attrWrapperP->value.firstChildP;
    while (instP != NULL)
    {
      CorNode* nextP = instP->next;
      bool    keep  = false;
      for (int i = 0; corNgsild.datasetIdV[i] != NULL; i++)
      {
        if (instP->name != NULL && strcmp(instP->name, corNgsild.datasetIdV[i]) == 0)
        {
          keep = true;
          break;
        }
      }
      if (!keep)
        corTreeChildRemove(attrWrapperP, instP);
      instP = nextP;
    }
    if (attrWrapperP->value.firstChildP == NULL)
    {
      ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
              "no matching datasetId for attribute '%s' in entity '%s'", attrWild, entityId);
      return true;
    }
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
  // wrap is now { attrName: {type, <valueMember>, ...} } — or, for a
  // multi-instance attribute, { attrName: [ {...}, {...} ] }. value-only returns
  // the default instance's value (datasetId already narrowed above); pick the
  // first instance when an array remains.
  //
  CorNode* attrP = wrap->value.firstChildP;
  if (attrP != NULL && attrP->type == CorArray)
    attrP = attrP->value.firstChildP;

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
  valueP->name = NULL;
  valueP->next = NULL;
  int   len = corJsonFastRenderSize(valueP) + 1;
  char* buf = (char*) kaAlloc(&corRest.kalloc, len);
  if (buf == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "out of memory rendering attribute value");
    return true;
  }
  corJsonFastRender(valueP, buf);

  corRest.out.payload        = buf;
  corRest.out.payloadSize    = strlen(buf);
  corRest.out.contentType    = (char*) corMimeString(CorMimeJson);
  corRest.out.httpStatusCode = 200;
  corNgsild.rawResponse      = true;
  return true;
}
