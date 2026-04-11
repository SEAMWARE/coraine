//
// FILE            patchEntity.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                  // NULL

#include "swRest/SwRestState.h"                      // swRest
#include "swNgsild/swNgsild.h"                       // ldError, ldCheckEntity, LdOp*, LD_ERROR_*, swNgsild
#include "swNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "swNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "swNgsild/ldEntityMerge.h"                  // LdMergeReport

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND

#include "serviceRoutines/patchEntity.h"             // Own interface



// -----------------------------------------------------------------------------
//
// patchEntity -
//
bool patchEntity(void)
{
  //
  // @context error detected in parseHook
  //
  if (swNgsild.contextError)
    return true;

  const char* entityId = swRest.in.wildcard[0];
  KjNode*     fragment = swRest.in.requestTree;

  //
  // Unsupported Content-Type (payload present but not parsed as JSON)
  //
  if (swRest.in.payload != NULL && fragment == NULL)
  {
    ldError(415, LD_ERROR_INVALID_REQUEST, "Unsupported Media Type",
            "supported Content-Types: application/json, application/ld+json");
    return true;
  }

  if (fragment == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "no payload");
    return true;
  }

  //
  // Validate the fragment. LdOpMergeEntity allows partial payloads (no
  // mandatory id/type) and permits "urn:ngsi-ld:null" at the top level as a
  // delete-marker.
  //
  if (ldCheckEntity(fragment, LdOpMergeEntity, NULL, &swRest.kalloc) == false)
    return true;

  //
  // Transform to DB model (dataset-keyed wrappers + timestamps). After this
  // call the fragment is in the same shape as the stored entity, which is
  // what ldEntityMerge expects.
  //
  ldApiEntityToDbModel(fragment, &swRest.kalloc);

  //
  // Apply the merge through the DB driver. The driver decides whether to
  // mutate in-place (ramdb) or round-trip through the wire (mongoc).
  //
  LdMergeReport report = { NULL };

  int r = db.entityMerge((Tenant*) swNgsild.tenantP, entityId, fragment, swRest.requestStartTime, &report);

  //
  // ldEntityMerge may have called ldError mid-merge (simplified LanguageProperty
  // without ?lang=, unsupported attribute type for a simplified scalar, ...).
  // That takes precedence over the driver's return code — otherwise a driver
  // that surfaces such errors as DB_ERR would mask the 400 with a 500.
  //
  if (swRest.out.problemType != NULL)
    return true;

  if (r == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity '%s' not found", entityId);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error merging entity '%s'", entityId);
    return true;
  }

  //
  // The report holds per-attribute change records (addedAttrs, modifiedAttrs,
  // deletedAttrs, preValues) needed by the subscription matcher. Subscriptions
  // are not wired yet — the matcher call will plug in here once they are.
  //
  (void) report;

  swRest.out.httpStatusCode = 204;
  return true;
}
