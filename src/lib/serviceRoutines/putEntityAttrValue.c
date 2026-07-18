//
// FILE            putEntityAttrValue.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// PUT /ngsi-ld/v1/entities/{entityId}/attrs/{attrId}/value
//
// Set Attribute Value (TS 104-176 § 7.6.3.2 / TS 104-175 § 10.2.6). The request
// body is a "value only" Attribute Fragment: the bare value itself (a JSON
// Primitive, Object or Array), NOT a JSON-LD document — so it is never
// @context-expanded (ldParseHook leaves it raw). The target Attribute must
// already exist; its type is preserved and only the value is replaced.
//
// Implemented by synthesizing the equivalent Attribute Fragment
// ({ <valueMember>: <body> }) from the existing attribute's type and delegating
// to patchEntityAttr — that reuses the merge (RFC 7396, preserves type +
// sub-attributes) and the distributed-operation forwarding (204 / 207).
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp

#include "swRest/SwRestState.h"                      // swRest

#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjChildAdd
#include "kjson/kjLookup.h"                          // kjLookup

#include "swJsonld/swldExpand.h"                     // swldExpand
#include "swJsonld/swldExpandTree.h"                 // swldExpandTree
#include "swJsonld/swldInit.h"                       // swldCoreContext

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldContextResolve
#include "swNgsild/LdVocab.h"                        // LD_VOCAB_SCOPE, LD_VOCAB_OBSERVED_AT, LD_VOCAB_NGSILD_NULL

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/patchEntityAttr.h"         // patchEntityAttr
#include "serviceRoutines/putEntityAttrValue.h"      // Own interface



// -----------------------------------------------------------------------------
//
// valueMemberForType - the value-holding member name for an attribute type
//
static const char* valueMemberForType(const char* typeStr)
{
  if (typeStr == NULL)                              return "value";
  if (strcmp(typeStr, "Relationship")     == 0)     return "object";
  if (strcmp(typeStr, "LanguageProperty") == 0)     return "languageMap";
  if (strcmp(typeStr, "VocabProperty")    == 0)     return "vocab";
  if (strcmp(typeStr, "JsonProperty")     == 0)     return "json";
  if (strcmp(typeStr, "ListProperty")     == 0)     return "valueList";
  if (strcmp(typeStr, "ListRelationship") == 0)     return "objectList";
  return "value";  // Property, GeoProperty
}



// -----------------------------------------------------------------------------
//
// putEntityAttrValue -
//
bool putEntityAttrValue(void)
{
  const char* entityId = swRest.in.wildcard[0];
  const char* attrWild = swRest.in.wildcard[1];
  KjNode*     valueP   = swRest.in.requestTree;  // raw value (NOT @context-expanded)

  if (valueP == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Request", "a value-only request body is required");
    return true;
  }

  // A value-only body is plain JSON — it carries no @context, so ld+json is
  // not a meaningful Content-Type for it.
  if (swRest.in.contentType != NULL && strstr(swRest.in.contentType, "ld+json") != NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Request",
            "a value-only body is plain JSON (no @context); use Content-Type application/json");
    return true;
  }

  ldContextResolve();

  SwldContext* ctxP    = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
  const char*  attrIri = swldExpand(ctxP, attrWild, &swRest.kalloc, NULL, NULL);
  if (attrIri == NULL) attrIri = attrWild;

  // § 10.2.6.4 — scope cannot be set this way.
  if (strcmp(attrWild, "scope") == 0 || strcmp(attrIri, LD_VOCAB_SCOPE) == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Immutable Field",
            "scope cannot be modified via Set Attribute Value");
    return true;
  }

  //
  // The target Attribute must already exist (value-only cannot create it, and
  // its type — which is preserved — tells us which member the value goes into).
  //
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;
  KjNode* entityP = NULL;
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

  KjNode* attrWrapperP = kjLookup(entityP, attrIri);
  if (attrWrapperP == NULL || attrWrapperP->type != KjObject)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "attribute '%s' not found in entity '%s'", attrWild, entityId);
    return true;
  }

  // Storage keys each instance by datasetId ("@none" for the default); the type
  // lives on the instance object.
  KjNode*     firstInstP = attrWrapperP->value.firstChildP;
  KjNode*     typeNodeP  = (firstInstP != NULL) ? kjLookup(firstInstP, "type") : NULL;
  const char* typeStr    = (typeNodeP != NULL && typeNodeP->type == KjString) ? typeNodeP->value.s : NULL;
  const char* member     = valueMemberForType(typeStr);

  // § 10.2.6.4 — if the target (default) instance previously carried an observedAt
  // sub-attribute, the value-only set updates it to the ?observedAt param when
  // supplied (handled by the merge's observedAt injection) or REMOVES it when no
  // ?observedAt is given. The removal is expressed with the NGSI-LD Null marker on
  // the synthesized fragment (§ 4.5.5.9).
  KjNode* targetInstP  = kjLookup(attrWrapperP, "@none");
  if (targetInstP == NULL) targetInstP = firstInstP;
  bool hadObservedAt = (targetInstP != NULL) && (kjLookup(targetInstP, LD_VOCAB_OBSERVED_AT) != NULL);

  // § 10.2.6.4 — a null value (or the urn:ngsi-ld:null sentinel) is invalid,
  // except for a JsonProperty whose json value may legitimately be null.
  bool isJsonProperty = (typeStr != NULL && strcmp(typeStr, "JsonProperty") == 0);
  if (!isJsonProperty)
  {
    if (valueP->type == KjNull ||
        (valueP->type == KjString && valueP->value.s != NULL && strcmp(valueP->value.s, "urn:ngsi-ld:null") == 0))
    {
      // § 10.2.6.4: a null / urn:ngsi-ld:null body raises InvalidRequest
      // specifically (not the generic BadRequestData).
      ldError(400, LD_ERROR_INVALID_REQUEST, "Invalid Request",
              "a null value-only body is not allowed for attribute '%s'", attrWild);
      return true;
    }
  }

  //
  // Synthesize the equivalent Attribute Fragment { type, <member>: <body> } and
  // hand it to patchEntityAttr. The existing type is echoed (§ 10.2.6.4 — the
  // type is preserved): it matches patchEntityAttr's type-change guard and keeps
  // the value-shape unambiguous (e.g. a GeoProperty's GeoJSON object must not be
  // mis-inferred as a Property). Sub-attributes are untouched by the merge.
  //
  KjNode* fragP = kjObject(swRest.kjsonP, NULL);
  if (typeStr != NULL)
    kjChildAdd(fragP, kjString(swRest.kjsonP, "type", typeStr));
  valueP->name = (char*) member;
  kjChildAdd(fragP, valueP);

  // § 10.2.6.4 observedAt: remove it when the attribute had one and no ?observedAt
  // is supplied. When ?observedAt IS supplied the merge injects it into the
  // fragment (ldEntityMerge), so nothing extra is needed for the update case.
  if (hadObservedAt && swNgsild.observedAt == NULL)
    kjChildAdd(fragP, kjString(swRest.kjsonP, LD_VOCAB_OBSERVED_AT, (char*) LD_VOCAB_NGSILD_NULL));

  // Expand the synthesized fragment the way ldParseHook would for a normal
  // PATCH /attrs body (the raw value-only body itself was deliberately not
  // expanded). This gives a GeoProperty's GeoJSON / a JsonProperty's json the
  // proper opaque-value treatment in ldApiEntityToDbModel; for a primitive it
  // is a no-op. The fragment has no @context, so the context is unchanged.
  swldExpandTree(fragP, ctxP, &swRest.kalloc);

  swRest.in.requestTree = fragP;

  return patchEntityAttr();  // merge + distops; 204 (or 207 on partial distop failure)
}
