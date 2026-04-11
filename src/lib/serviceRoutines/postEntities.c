//
// FILE            postEntities.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <string.h>                                  // strlen, strcpy, strcat

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/KjNode.h"                            // KjNode, KjString
#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "swJsonld/swldInit.h"                       // swldCoreContext
#include "swJsonld/SwldContext.h"                    // SwldContext
#include "swNgsild/swNgsild.h"                       // ldError, ldCheckEntity, LdOp, LD_ERROR_*, swNgsild
#include "swNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "swNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel

#include "db/DbDriver.h"                             // db, DB_OK, DB_ALREADY_EXISTS

#include "serviceRoutines/postEntities.h"            // Own interface



// -----------------------------------------------------------------------------
//
// postEntities -
//
bool postEntities(void)
{
  //
  // @context error detected in parseHook
  //
  if (swNgsild.contextError)
    return true;

  KjNode* entityP = swRest.in.requestTree;

  //
  // Unsupported Content-Type (payload present but not parsed as JSON)
  //
  if (swRest.in.payload != NULL && entityP == NULL)
  {
    ldError(415, LD_ERROR_INVALID_REQUEST, "Unsupported Media Type",
            "supported Content-Types: application/json, application/ld+json");
    return true;
  }

  //
  // Must have a JSON payload
  //
  if (entityP == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "no payload");
    return true;
  }

  //
  // Validate the entity
  //
  if (ldCheckEntity(entityP, LdOpCreateEntity, NULL, &swRest.kalloc) == false)
    return true;

  //
  // Extract entity id
  //
  KjNode* idP = kjLookup(entityP, "id");

  if (idP == NULL || idP->type != KjString)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "entity id is missing or not a string");
    return true;
  }

  //
  // Transform to DB model (dataset-keyed wrappers + timestamps)
  //
  ldApiEntityToDbModel(entityP, &swRest.kalloc);

  //
  // Create entity in database
  //
  int r = db.entityCreate((Tenant*) swNgsild.tenantP, idP->value.s, entityP);

  if (r == DB_ALREADY_EXISTS)
  {
    ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists", "entity '%s' already exists", idP->value.s);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error creating entity '%s'", idP->value.s);
    return true;
  }

  //
  // 201 Created -- set Location and Link headers, no body
  //
  swRest.out.httpStatusCode = 201;

  SwRestKeyValue* hV = swRest.out.headerV;
  int ix = swRest.out.headerCount;

  hV[ix].key   = "Location";
  hV[ix].value = idP->value.s;
  ix++;

  SwldContext* ctxP = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
  const char*  ctxUrl = ctxP->url;

  if (ctxUrl != NULL)
  {
    const char* suffix  = ">; rel=\"http://www.w3.org/ns/json-ld#context\"; type=\"application/ld+json\"";
    int         linkLen = 1 + strlen(ctxUrl) + strlen(suffix) + 1;
    char*       linkBuf = kaAlloc(&swRest.kalloc, linkLen);

    strcpy(linkBuf, "<");
    strcat(linkBuf, ctxUrl);
    strcat(linkBuf, suffix);
    hV[ix].key   = "Link";
    hV[ix].value = linkBuf;
    ix++;
  }

  swRest.out.headerCount = ix;

  return true;
}
