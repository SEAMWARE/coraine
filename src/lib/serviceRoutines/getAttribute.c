//
// FILE            getAttribute.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/attributes/{attrId} — Retrieve Available Attribute
// Information (§ 5.7.10). Returns full Attribute (§ 5.2.28): id, type,
// attributeName, attributeCount, attributeTypes, typeNames.
//
// Mode 1 only (local). Modes 2/3 to follow.
//

#include <stddef.h>                                   // NULL
#include <string.h>                                   // strcmp

#include "swRest/SwRestState.h"                       // swRest
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjArray, kjObject, kjString, kjInteger, kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup

#include "swJsonld/swldCompact.h"                     // swldCompact
#include "swJsonld/swldExpand.h"                      // swldExpand
#include "swJsonld/swldInit.h"                        // swldCoreContext

#include "swNgsild/swNgsild.h"                        // ldError, LD_ERROR_*, swNgsild

#include "db/DbDriver.h"                              // db, DB_OK
#include "db/Tenant.h"                                // Tenant

#include "serviceRoutines/getAttribute.h"             // Own interface



static const char* shortOrSelf(SwldContext* ctxP, const char* iri)
{
  const char* compact = swldCompact(ctxP, iri);
  return (compact != NULL) ? compact : iri;
}



bool getAttribute(void)
{
  if (swNgsild.contextError)
    return true;

  Tenant*     tenantP  = (Tenant*) swNgsild.tenantP;
  const char* attrWild = swRest.in.wildcard[0];

  if (db.attrList == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented",
            "attribute discovery not supported by this DB plugin");
    return true;
  }

  SwldContext* ctxP = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();

  const char* attrIri = swldExpand(ctxP, attrWild, &swRest.kalloc, NULL, NULL);
  if (attrIri == NULL)
    attrIri = attrWild;

  KjNode* aggregated = NULL;
  int r = db.attrList(tenantP, true, &aggregated);
  if (r != DB_OK || aggregated == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "attribute discovery failed");
    return true;
  }

  KjNode* entry = NULL;
  for (KjNode* e = aggregated->value.firstChildP; e != NULL; e = e->next)
  {
    KjNode* iriP = kjLookup(e, "attrIri");
    if (iriP != NULL && iriP->type == KjString && strcmp(iriP->value.s, attrIri) == 0)
    {
      entry = e;
      break;
    }
  }

  if (entry == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "attribute '%s' not found", attrWild);
    return true;
  }

  //
  // Attribute (§ 5.2.28): id, type, attributeName, attributeCount,
  //                       attributeTypes, typeNames
  //
  KjNode* body = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(body, kjString(swRest.kjsonP, "id",            attrIri));
  kjChildAdd(body, kjString(swRest.kjsonP, "type",          "Attribute"));
  kjChildAdd(body, kjString(swRest.kjsonP, "attributeName", shortOrSelf(ctxP, attrIri)));

  KjNode* countP = kjLookup(entry, "attrCount");
  kjChildAdd(body, kjInteger(swRest.kjsonP, "attributeCount",
                             (countP != NULL) ? countP->value.i : 0));

  KjNode* at = kjArray(swRest.kjsonP, "attributeTypes");
  KjNode* atSrc = kjLookup(entry, "attrTypes");
  if (atSrc != NULL && atSrc->type == KjArray)
  {
    for (KjNode* t = atSrc->value.firstChildP; t != NULL; t = t->next)
      if (t->type == KjString)
        kjChildAdd(at, kjString(swRest.kjsonP, NULL, t->value.s));
  }
  kjChildAdd(body, at);

  KjNode* tn = kjArray(swRest.kjsonP, "typeNames");
  KjNode* tnSrc = kjLookup(entry, "typeNames");
  if (tnSrc != NULL && tnSrc->type == KjArray)
  {
    for (KjNode* t = tnSrc->value.firstChildP; t != NULL; t = t->next)
      if (t->type == KjString)
        kjChildAdd(tn, kjString(swRest.kjsonP, NULL, shortOrSelf(ctxP, t->value.s)));
  }
  kjChildAdd(body, tn);

  swRest.out.responseTree   = body;
  swRest.out.httpStatusCode = 200;
  return true;
}
