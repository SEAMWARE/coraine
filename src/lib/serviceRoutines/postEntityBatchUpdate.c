//
// FILE            postEntityBatchUpdate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/entityOperations/update — Batch Entity Update (§ 5.6.8).
//
// Semantics match Update Attributes (§ 5.6.3) per-entity, extended to a
// list. Multiple instances of the same entity id in one batch are
// **combined in array order** — first element is oldest, last is
// newest — so the merge sequence is deterministic even though every
// instance shares the same wall-clock modifiedAt. Notifications for
// intermediate merged states are queued via ldNotifyDefer and emitted
// post-response as one Notification per subscription with data[]
// carrying every matched state in encounter order.
//
// Flow:
//   Pass 1 — validate + group by id (preserving array order within a group).
//   Pass 2 — per group: retrieve existing, merge each fragment in order
//            (stamping ts = request start), defer one notification
//            candidate per merged state.
//   Pass 3 — bulk DB write of final merged states via db.entityBulkUpdate.
//   Pass 4 — response: BatchOperationResult (§ 5.2.17) — 204 all-OK,
//            207 partial, 409 all-failed.
//
// Distops forwarding (§ 5.6.8.4 — "we must always try to forward with
// minimal changes") is left for a follow-up; local-only semantics
// here.
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp, strlen

#include "swRest/SwRestState.h"                      // swRest

#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjArray, kjString, kjChildAdd
#include "kjson/kjClone.h"                           // kjClone
#include "kjson/kjLookup.h"                          // kjLookup

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdOp.h"                           // LdOpUpdateAttrs
#include "swNgsild/LdNormalizeInput.h"               // ldNormalizeInput
#include "swNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "swNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "swNgsild/LdProblem.h"                      // LD_ERROR_RESOURCE_NOT_FOUND, LD_ERROR_INTERNAL_ERROR
#include "swNgsild/ldEntityAttrsSet.h"               // ldEntityAttrsSet
#include "swNgsild/ldEntityMerge.h"                  // LdMergeReport
#include "swNgsild/ldSubscriptionNotify.h"           // LdNotifyEntityUpdate
#include "swNgsild/ldNotifyDefer.h"                  // ldNotifyDefer
#include "swNgsild/LdSubCache.h"                     // LdSubCache

#include "db/DbDriver.h"                             // db, DB_OK, DB_NOT_FOUND, DB_ERR
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postEntityBatchUpdate.h"   // Own interface



// -----------------------------------------------------------------------------
//
// addBatchError - append a BatchEntityError (§ 5.2.17) to errors[].
//
static void addBatchError(KjNode* errorsP, const char* entityId,
                          const char* errType, const char* title,
                          const char* detail)
{
  KjNode* err = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(err, kjString(swRest.kjsonP, "entityId", (char*) entityId));

  KjNode* pd = kjObject(swRest.kjsonP, "error");
  kjChildAdd(pd, kjString(swRest.kjsonP, "type",   (char*) errType));
  kjChildAdd(pd, kjString(swRest.kjsonP, "title",  (char*) title));
  kjChildAdd(pd, kjString(swRest.kjsonP, "detail", (char*) detail));
  kjChildAdd(err, pd);

  kjChildAdd(errorsP, err);
}



// -----------------------------------------------------------------------------
//
// Group - set of fragments targeting one entity id, in arrival order.
//
typedef struct Group
{
  const char*  id;
  KjNode**     fragV;
  int          count;
  int          capacity;
} Group;



static Group* groupFindOrCreate(Group** groupsP, int* gNp, int* gCapP, const char* id)
{
  for (int i = 0; i < *gNp; i++)
    if (strcmp((*groupsP)[i].id, id) == 0)
      return &(*groupsP)[i];

  if (*gNp >= *gCapP)
  {
    int newCap = (*gCapP == 0) ? 8 : *gCapP * 2;
    Group* newV = (Group*) kaAlloc(&swRest.kalloc, newCap * sizeof(Group));
    for (int i = 0; i < *gNp; i++) newV[i] = (*groupsP)[i];
    *groupsP = newV;
    *gCapP   = newCap;
  }

  (*groupsP)[*gNp].id       = id;
  (*groupsP)[*gNp].fragV    = NULL;
  (*groupsP)[*gNp].count    = 0;
  (*groupsP)[*gNp].capacity = 0;
  return &(*groupsP)[(*gNp)++];
}



static void groupFragAppend(Group* g, KjNode* fragP)
{
  if (g->count >= g->capacity)
  {
    int newCap = (g->capacity == 0) ? 4 : g->capacity * 2;
    KjNode** newV = (KjNode**) kaAlloc(&swRest.kalloc, newCap * sizeof(KjNode*));
    for (int i = 0; i < g->count; i++) newV[i] = g->fragV[i];
    g->fragV    = newV;
    g->capacity = newCap;
  }
  g->fragV[g->count++] = fragP;
}



// -----------------------------------------------------------------------------
//
// postEntityBatchUpdate -
//
bool postEntityBatchUpdate(void)
{
  if (swNgsild.contextError)
    return true;

  KjNode* bodyP = swRest.in.requestTree;

  if (swRest.in.payload != NULL && bodyP == NULL)
  {
    ldError(415, LD_ERROR_INVALID_REQUEST, "Unsupported Media Type",
            "supported Content-Types: application/json, application/ld+json");
    return true;
  }

  if (bodyP == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "no payload");
    return true;
  }

  if (bodyP->type != KjArray)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Batch Entity Update body must be a JSON array");
    return true;
  }

  int total = 0;
  for (KjNode* c = bodyP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->type == KjNull)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "Batch Entity Update: null entry at position %d", total);
      return true;
    }
    total++;
  }

  if (total == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Batch Entity Update: input array is empty");
    return true;
  }

  KjNode* successP = kjArray(swRest.kjsonP, "success");
  KjNode* errorsP  = kjArray(swRest.kjsonP, "errors");

  //
  // Pass 1 — validate, normalise, group by id (preserving array order
  // of fragments within each group).
  //
  Group* groups = NULL;
  int    gN     = 0;
  int    gCap   = 0;

  for (KjNode* inP = bodyP->value.firstChildP; inP != NULL; inP = inP->next)
  {
    if (inP->type != KjObject)
    {
      addBatchError(errorsP, "",
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
                    "entity must be a JSON object");
      continue;
    }

    ldNormalizeInput(inP, &swRest.kalloc, false);

    if (ldCheckEntity(inP, LdOpUpdateAttrs, NULL, &swRest.kalloc) == false)
    {
      const char* eid = "";
      KjNode* idP = kjLookup(inP, "id");
      if (idP != NULL && idP->type == KjString) eid = idP->value.s;

      char snapshot[512];
      strncpy(snapshot, swRest.out.problemDetail, sizeof(snapshot) - 1);
      snapshot[sizeof(snapshot) - 1] = 0;

      addBatchError(errorsP, eid,
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request", snapshot);

      swRest.out.httpStatusCode   = 0;
      swRest.out.problemType      = NULL;
      swRest.out.problemTitle     = NULL;
      swRest.out.problemDetail[0] = 0;
      continue;
    }

    KjNode* idP = kjLookup(inP, "id");
    if (idP == NULL || idP->type != KjString)
    {
      addBatchError(errorsP, "",
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
                    "entity id is missing or not a string");
      continue;
    }

    Group* g = groupFindOrCreate(&groups, &gN, &gCap, idP->value.s);
    groupFragAppend(g, inP);
  }

  if (gN == 0)
  {
    KjNode* respBodyP = kjObject(swRest.kjsonP, NULL);
    kjChildAdd(respBodyP, successP);
    kjChildAdd(respBodyP, errorsP);
    swRest.out.responseTree   = respBodyP;
    swRest.out.httpStatusCode = 400;
    swNgsild.rawResponse      = true;
    return true;
  }

  //
  // Pass 2 — per group: retrieve, merge each fragment in order, defer
  // one notification candidate per merged state.
  //
  Tenant*      tenantP   = (Tenant*) swNgsild.tenantP;
  LdSubCache*  subCacheP = (tenantP != NULL) ? (LdSubCache*) tenantP->subCacheP : NULL;

  KjNode*      finals     = kjArray(swRest.kjsonP, NULL);
  const char** finalIdV   = (const char**) kaAlloc(&swRest.kalloc, sizeof(char*) * gN);
  int          finalN     = 0;

  for (int gi = 0; gi < gN; gi++)
  {
    Group* g = &groups[gi];

    //
    // Retrieve current entity state from DB (storage format).
    //
    KjNode* existingDb = NULL;
    if (db.entityRetrieve == NULL)
    {
      addBatchError(errorsP, g->id,
                    LD_ERROR_INTERNAL_ERROR, "Internal Error",
                    "entityRetrieve not supported by this DB plugin");
      continue;
    }

    int r = db.entityRetrieve(tenantP, g->id, &existingDb);
    if (r == DB_NOT_FOUND || existingDb == NULL)
    {
      addBatchError(errorsP, g->id,
                    LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                    "entity does not exist");
      continue;
    }
    if (r != DB_OK)
    {
      addBatchError(errorsP, g->id,
                    LD_ERROR_INTERNAL_ERROR, "Internal Error",
                    "database error during retrieve");
      continue;
    }

    //
    // Apply each fragment in array order, accumulating notifications
    // for each intermediate merged state. The last merged state is the
    // one that reaches the DB via bulk update.
    //
    bool anyMerge = false;
    for (int fi = 0; fi < g->count; fi++)
    {
      KjNode* fragP = g->fragV[fi];

      // Fragment is API-form (from request). Storage-format existingDb
      // expects DB-form fragments, so convert the fragment in place.
      ldApiEntityToDbModel(fragP, &swRest.kalloc);

      LdMergeReport report = { NULL };
      ldEntityAttrsSet(existingDb, fragP, true /* overwriteScope */,
                       swRest.requestStartTime, &report, swRest.kjsonP);
      anyMerge = true;

      //
      // Defer a notification candidate: clone the current merged state
      // so subsequent merges in this loop don't mutate the pending data.
      //
      if (subCacheP != NULL)
      {
        KjNode* snapshot = kjClone(swRest.kjsonP, existingDb);
        ldNotifyDefer(subCacheP, snapshot, LdNotifyEntityUpdate, &report);
      }
    }

    if (anyMerge)
    {
      finalIdV[finalN++] = g->id;
      kjChildAdd(finals, existingDb);
    }
  }

  //
  // Pass 3 — bulk DB write of final states.
  //
  if (finalN > 0)
  {
    if (db.entityBulkUpdate == NULL)
    {
      ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented",
              "Batch Entity Update not supported by this DB plugin");
      return true;
    }

    int* resultsV = (int*) kaAlloc(&swRest.kalloc, sizeof(int) * finalN);
    db.entityBulkUpdate(tenantP, finals, resultsV);

    for (int k = 0; k < finalN; k++)
    {
      const char* eid = finalIdV[k];
      switch (resultsV[k])
      {
        case DB_OK:
          kjChildAdd(successP, kjString(swRest.kjsonP, NULL, (char*) eid));
          break;
        case DB_NOT_FOUND:
          addBatchError(errorsP, eid,
                        LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
                        "entity vanished between retrieve and bulk update");
          break;
        default:
          addBatchError(errorsP, eid,
                        LD_ERROR_INTERNAL_ERROR, "Internal Error",
                        "database error during batch update");
          break;
      }
    }
  }

  //
  // Pass 4 — response assembly.
  //
  int successCount = 0;
  for (KjNode* p = successP->value.firstChildP; p != NULL; p = p->next) successCount++;

  int errorCount = 0;
  for (KjNode* p = errorsP->value.firstChildP; p != NULL; p = p->next) errorCount++;

  if (errorCount == 0)
  {
    // All-ok → 204 No Content, no body.
    swRest.out.httpStatusCode = 204;
    return true;
  }

  KjNode* respBodyP = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(respBodyP, successP);
  kjChildAdd(respBodyP, errorsP);
  swRest.out.responseTree = respBodyP;
  swRest.out.httpStatusCode = (successCount > 0) ? 207 : 409;
  swNgsild.rawResponse      = true;
  return true;
}
