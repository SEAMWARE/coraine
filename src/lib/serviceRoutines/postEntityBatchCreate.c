//
// FILE            postEntityBatchCreate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// POST /ngsi-ld/v1/entityOperations/create — Batch Entity Creation (§ 5.6.7).
//
// Body: JSON array of one or more entities (§ 5.6.7.3). Per-entity
// behavior per § 5.5.11.1 — first occurrence wins, subsequent duplicates
// of the same id yield an AlreadyExists error in the response body.
//
// Response: BatchOperationResult (§ 5.2.17) — 201 Created on all-OK, 207
// Multi-Status on partial, 409 Conflict on complete failure.
//
// Distops forwarding per § 5.6.7.4 is deferred — this first cut is
// local-only. mongoc uses a single insert_many round-trip via
// db.entityBulkCreate.
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp

#include "swRest/SwRestState.h"                      // swRest

#include "kalloc/kaAlloc.h"                          // kaAlloc
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjArray, kjString, kjChildAdd
#include "kjson/kjLookup.h"                          // kjLookup

#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdOp.h"                           // LdOpCreateEntity
#include "swNgsild/LdNormalizeInput.h"               // ldNormalizeInput
#include "swNgsild/ldCheckEntity.h"                  // ldCheckEntity
#include "swNgsild/ldApiEntityToDbModel.h"           // ldApiEntityToDbModel
#include "swNgsild/LdProblem.h"                      // LD_ERROR_ALREADY_EXISTS, LD_ERROR_BAD_REQUEST_DATA

#include "db/DbDriver.h"                             // db, DB_OK, DB_ALREADY_EXISTS, DB_ERR
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/postEntityBatchCreate.h"   // Own interface



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
// postEntityBatchCreate -
//
bool postEntityBatchCreate(void)
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

  //
  // § 5.6.7.3: body must be an array. § 5.6.7.4: empty array or null
  // element → BadRequestData.
  //
  if (bodyP->type != KjArray)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Batch Entity Creation body must be a JSON array");
    return true;
  }

  int total = 0;
  for (KjNode* c = bodyP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->type == KjNull)
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
              "Batch Entity Creation: null entry at position %d", total);
      return true;
    }
    total++;
  }

  if (total == 0)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
            "Batch Entity Creation: input array is empty");
    return true;
  }

  KjNode* successP = kjArray(swRest.kjsonP, "success");
  KjNode* errorsP  = kjArray(swRest.kjsonP, "errors");

  //
  // Pass 1: walk the input array, validate + normalize + dedup. Entities
  // that pass all checks get moved into eligibleP (and out of bodyP).
  // eligIdV keeps a parallel vector of IDs aligned with eligibleP's
  // order — we need it to map bulk-insert results back to entity ids.
  //
  KjNode*      eligibleP = kjArray(swRest.kjsonP, NULL);
  const char** eligIdV   = (const char**) kaAlloc(&swRest.kalloc, sizeof(char*) * total);
  int          eligN     = 0;

  // Seen-ids tracker for § 5.5.11.1 (first-wins within batch).
  KjNode* seen = kjObject(swRest.kjsonP, NULL);

  KjNode* inP = bodyP->value.firstChildP;
  while (inP != NULL)
  {
    KjNode* nextP = inP->next;   // capture BEFORE we potentially detach

    if (inP->type != KjObject)
    {
      addBatchError(errorsP, "",
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
                    "entity must be a JSON object");
      inP = nextP;
      continue;
    }

    //
    // ldHooks did JSON-LD expansion but skipped normalization for
    // non-/entities URLs. Normalize each entity here: simplified /
    // concise → normalized.
    //
    ldNormalizeInput(inP, &swRest.kalloc, false);

    //
    // ldCheckEntity sets swRest.out.problemType / problemDetail on
    // failure. For a batch we want per-entity errors, so snapshot the
    // detail, clear the error flag, and continue the loop.
    //
    if (ldCheckEntity(inP, LdOpCreateEntity, NULL, &swRest.kalloc) == false)
    {
      const char* eid = "";
      KjNode* idP = kjLookup(inP, "id");
      if (idP != NULL && idP->type == KjString) eid = idP->value.s;

      char snapshot[512];
      strncpy(snapshot, swRest.out.problemDetail, sizeof(snapshot) - 1);
      snapshot[sizeof(snapshot) - 1] = 0;

      addBatchError(errorsP, eid,
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request", snapshot);

      // Clear error flags so the next entity's validation starts clean.
      swRest.out.httpStatusCode = 0;
      swRest.out.problemType    = NULL;
      swRest.out.problemTitle   = NULL;
      swRest.out.problemDetail[0] = 0;

      inP = nextP;
      continue;
    }

    KjNode* idP = kjLookup(inP, "id");
    if (idP == NULL || idP->type != KjString)
    {
      addBatchError(errorsP, "",
                    LD_ERROR_BAD_REQUEST_DATA, "Bad Request",
                    "entity id is missing or not a string");
      inP = nextP;
      continue;
    }

    const char* eid = idP->value.s;

    if (kjLookup(seen, eid) != NULL)
    {
      addBatchError(errorsP, eid,
                    LD_ERROR_ALREADY_EXISTS, "Already Exists",
                    "duplicate entity id within the same batch");
      inP = nextP;
      continue;
    }
    kjChildAdd(seen, kjString(swRest.kjsonP, eid, ""));

    ldApiEntityToDbModel(inP, &swRest.kalloc);

    eligIdV[eligN] = eid;
    eligN++;
    kjChildAdd(eligibleP, inP);   // this nulls inP->next — nextP already captured

    inP = nextP;
  }

  //
  // Pass 2: bulk insert. db.entityBulkCreate is a single round-trip for
  // mongoc; ramdb loops internally.
  //
  if (eligN > 0)
  {
    if (db.entityBulkCreate == NULL)
    {
      ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented",
              "Batch Entity Creation not supported by this DB plugin");
      return true;
    }

    Tenant* tenantP  = (Tenant*) swNgsild.tenantP;
    int*    resultsV = (int*) kaAlloc(&swRest.kalloc, sizeof(int) * eligN);

    db.entityBulkCreate(tenantP, eligibleP, resultsV);

    for (int i = 0; i < eligN; i++)
    {
      const char* eid = eligIdV[i];
      switch (resultsV[i])
      {
        case DB_OK:
          kjChildAdd(successP, kjString(swRest.kjsonP, NULL, (char*) eid));
          break;
        case DB_ALREADY_EXISTS:
          addBatchError(errorsP, eid,
                        LD_ERROR_ALREADY_EXISTS, "Already Exists",
                        "entity already exists");
          break;
        default:
          addBatchError(errorsP, eid,
                        LD_ERROR_INTERNAL_ERROR, "Internal Error",
                        "database error during batch insert");
          break;
      }
    }
  }

  int successCount = 0;
  for (KjNode* p = successP->value.firstChildP; p != NULL; p = p->next) successCount++;

  int errorCount = 0;
  for (KjNode* p = errorsP->value.firstChildP; p != NULL; p = p->next) errorCount++;

  KjNode* respBodyP = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(respBodyP, successP);
  kjChildAdd(respBodyP, errorsP);
  swRest.out.responseTree = respBodyP;

  if (errorCount == 0)
    swRest.out.httpStatusCode = 201;
  else if (successCount > 0)
    swRest.out.httpStatusCode = 207;
  else
    swRest.out.httpStatusCode = 409;

  // Pure BatchOperationResult — renderHook's entity-attr transforms
  // would corrupt it. rawResponse bypasses those.
  swNgsild.rawResponse = true;

  return true;
}
