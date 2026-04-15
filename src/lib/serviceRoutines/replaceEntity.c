//
// FILE            replaceEntity.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// PUT /ngsi-ld/v1/entities/{entityId} — Replace Entity.
// NGSI-LD v1.9.1 §5.6.16 / §5.5.12.
//

#include <stddef.h>                                   // NULL
#include <string.h>                                   // strcmp

#include "swRest/SwRestState.h"                       // swRest
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/KjNode.h"                             // KjNode
#include "swNgsild/swNgsild.h"                        // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/ldCheckEntity.h"                   // ldCheckEntity
#include "swNgsild/LdOp.h"                            // LdOpReplaceEntity
#include "swNgsild/ldApiEntityToDbModel.h"            // ldApiEntityToDbModel
#include "swNgsild/LdSubCache.h"                      // LdSubCache
#include "swNgsild/ldSubscriptionNotify.h"            // LdNotifyEntityUpdate
#include "swNgsild/ldNotifyDefer.h"                   // ldNotifyDefer

#include "db/DbDriver.h"                              // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                                // Tenant

#include "serviceRoutines/replaceEntity.h"            // Own interface



// -----------------------------------------------------------------------------
//
// typeEqual - compare two NGSI-LD entity "type" nodes for equality.
//
// Both must be the same shape (string vs. array) and carry the same set of
// values. String comparison is exact (the values are expanded IRIs, so a
// mismatch here is a real type change).
//
static bool typeEqual(KjNode* a, KjNode* b)
{
  if ((a == NULL) || (b == NULL))
    return false;

  if ((a->type == KjString) && (b->type == KjString))
    return strcmp(a->value.s, b->value.s) == 0;

  if ((a->type != KjArray) || (b->type != KjArray))
    return false;

  //
  // Both arrays — require identical sets (any order).
  //
  for (KjNode* aI = a->value.firstChildP; aI != NULL; aI = aI->next)
  {
    if (aI->type != KjString)
      return false;

    bool found = false;
    for (KjNode* bI = b->value.firstChildP; bI != NULL; bI = bI->next)
    {
      if ((bI->type == KjString) && (strcmp(aI->value.s, bI->value.s) == 0))
      {
        found = true;
        break;
      }
    }

    if (!found)
      return false;
  }

  for (KjNode* bI = b->value.firstChildP; bI != NULL; bI = bI->next)
  {
    if (bI->type != KjString)
      return false;

    bool found = false;
    for (KjNode* aI = a->value.firstChildP; aI != NULL; aI = aI->next)
    {
      if ((aI->type == KjString) && (strcmp(bI->value.s, aI->value.s) == 0))
      {
        found = true;
        break;
      }
    }

    if (!found)
      return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// replaceEntity -
//
bool replaceEntity(void)
{
  //
  // @context error detected in parseHook
  //
  if (swNgsild.contextError)
    return true;

  const char* entityId = swRest.in.wildcard[0];
  KjNode*     entityP  = swRest.in.requestTree;

  //
  // Unsupported Content-Type (payload present but not parsed as JSON)
  //
  if (swRest.in.payload != NULL && entityP == NULL)
  {
    ldError(415, LD_ERROR_INVALID_REQUEST, "Unsupported Media Type",
            "supported Content-Types: application/json, application/ld+json");
    return true;
  }

  if (entityP == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "no payload");
    return true;
  }

  //
  // Validate payload as a full entity (ldOpReplaceEntity is a create-op
  // for validation purposes: id + type mandatory, no null-marker allowed).
  //
  if (ldCheckEntity(entityP, LdOpReplaceEntity, NULL, &swRest.kalloc) == false)
    return true;

  //
  // Id consistency: body id (if present) must match URL id
  //
  KjNode* bodyIdP = kjLookup(entityP, "id");
  if (bodyIdP != NULL && bodyIdP->type == KjString && strcmp(bodyIdP->value.s, entityId) != 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "entity id in payload ('%s') does not match URL ('%s')",
            bodyIdP->value.s, entityId);
    return true;
  }

  //
  // Transform to DB model (dataset-keyed wrappers + timestamps). After this
  // the entity has the same shape as the stored form, which is what
  // entityReplace expects.
  //
  ldApiEntityToDbModel(entityP, &swRest.kalloc);

  //
  // Type-change guard: retrieve the existing entity so we can compare its
  // top-level "type" against the new one. Retrieve also gives us the 404
  // signal when the entity doesn't exist. The atomic entityReplace below
  // still returns DB_NOT_FOUND if a concurrent DELETE slipped in between,
  // so the race window is narrow.
  //
  Tenant* tenantP   = (Tenant*) swNgsild.tenantP;
  KjNode* oldStored = NULL;

  int rr = db.entityRetrieve(tenantP, entityId, &oldStored);

  if (rr == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity '%s' not found", entityId);
    return true;
  }

  if (rr != DB_OK || oldStored == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error retrieving entity '%s'", entityId);
    return true;
  }

  KjNode* newTypeP = kjLookup(entityP, "type");
  KjNode* oldTypeP = kjLookup(oldStored, "type");

  if (!typeEqual(newTypeP, oldTypeP))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "entity type cannot be changed on Replace");
    return true;
  }

  //
  // Atomic replace at the driver level.
  //
  if (db.entityReplace == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented",
            "Replace Entity not supported by this DB plugin");
    return true;
  }

  KjNode* replacedOld = NULL;
  int     r           = db.entityReplace(tenantP, entityId, entityP, &replacedOld);

  if (r == DB_NOT_FOUND)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found", "entity '%s' not found", entityId);
    return true;
  }

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error replacing entity '%s'", entityId);
    return true;
  }

  //
  // Notification.
  // mongoc's entityReplace → mongocKjTreeToBson renames "id" to "_id" in
  // place. Restore before handing the tree to the notifier.
  //
  if (bodyIdP != NULL && bodyIdP->name[0] == '_')
    bodyIdP->name = "id";

  if (tenantP->subCacheP != NULL)
    ldNotifyDefer((LdSubCache*) tenantP->subCacheP, entityP, LdNotifyEntityUpdate, NULL);

  swRest.out.httpStatusCode = 204;
  return true;
}
