//
// FILE            postCsourceRegistration.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/csourceRegistrations  (NGSI-LD § 5.9.2)
//

#include <string.h>                                  // strlen, strcpy, strcat
#include <stdio.h>                                   // snprintf
#include <time.h>                                    // time

#include "swRest/SwRestState.h"                      // swRest
#include "kjson/kjLookup.h"                          // kjLookup
#include "kjson/kjBuilder.h"                         // kjString, kjChildAdd
#include "kjson/KjNode.h"                            // KjNode
#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "swJsonld/swldInit.h"                       // swldCoreContext
#include "swJsonld/SwldContext.h"                    // SwldContext
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldCheckRegistration.h"            // ldCheckRegistration
#include "swNgsild/LdOp.h"                           // LdOpCreateRegistration
#include "swNgsild/LdRegCache.h"                     // LdRegCache
#include "swNgsild/ldRegCache.h"                     // ldRegCacheItemAdd

#include "db/DbDriver.h"                             // db, DB_OK, DB_ALREADY_EXISTS
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postCsourceRegistration.h" // Own interface



// -----------------------------------------------------------------------------
//
// regIdGenerate - generate a registration id when none is provided
//
static char* regIdGenerate(KAlloc* allocP)
{
  static int counter = 0;
  char*      buf     = kaAlloc(allocP, 128);

  snprintf(buf, 128, "urn:ngsi-ld:ContextSourceRegistration:%lx:%04x", (long) time(NULL), ++counter & 0xFFFF);

  return buf;
}



// -----------------------------------------------------------------------------
//
// postCsourceRegistration -
//
bool postCsourceRegistration(void)
{
  if (swNgsild.contextError)
    return true;

  KjNode* regP = swRest.in.requestTree;

  if (swRest.in.payload != NULL && regP == NULL)
  {
    ldError(415, LD_ERROR_INVALID_REQUEST, "Unsupported Media Type",
            "supported Content-Types: application/json, application/ld+json");
    return true;
  }

  if (regP == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "no payload");
    return true;
  }

  // Validate the registration
  if (ldCheckRegistration(regP, LdOpCreateRegistration, &swRest.kalloc) == false)
    return true;

  // Extract or generate registration id
  KjNode* idP = kjLookup(regP, "id");

  if (idP == NULL)
  {
    char* generatedId = regIdGenerate(&swRest.kalloc);

    idP = kjString(NULL, "id", generatedId);
    kjChildAdd(regP, idP);
  }
  else if (idP->type != KjString)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "registration 'id' must be a string");
    return true;
  }
  else if (idP->value.s[0] == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "registration 'id' must not be empty");
    return true;
  }
  else if (ldCheckUri(idP->value.s) == false)
  {
    return true;  // ldCheckUri already raised the ldError
  }

  // Create registration in the database
  if (db.registrationCreate == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented", "registration CRUD not supported by this DB plugin");
    return true;
  }

  int r = db.registrationCreate((Tenant*) swNgsild.tenantP, idP->value.s, regP);

  if (r == DB_ALREADY_EXISTS)
  {
    ldError(409, LD_ERROR_ALREADY_EXISTS, "Already Exists", "registration '%s' already exists", idP->value.s);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error creating registration '%s'", idP->value.s);
    return true;
  }

  // Restore "id" key if mongocKjTreeToBson renamed it to "_id" in-place
  if (idP->name[0] == '_')
    idP->name = "id";

  // Add to per-tenant registration cache
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;
  if (tenantP->regCacheP != NULL)
    ldRegCacheItemAdd((LdRegCache*) tenantP->regCacheP, regP);

  // 201 Created — set Location and Link headers, no body
  swRest.out.httpStatusCode = 201;

  SwRestKeyValue* hV = swRest.out.headerV;
  int             ix = swRest.out.headerCount;

  const char* prefix = "/ngsi-ld/v1/csourceRegistrations/";
  int         locLen = strlen(prefix) + strlen(idP->value.s) + 1;
  char*       locBuf = kaAlloc(&swRest.kalloc, locLen);

  strcpy(locBuf, prefix);
  strcat(locBuf, idP->value.s);
  hV[ix].key   = "Location";
  hV[ix].value = locBuf;
  ix++;

  SwldContext* ctxP   = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();
  const char*  ctxUrl = (ctxP != NULL) ? ctxP->url : NULL;

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
