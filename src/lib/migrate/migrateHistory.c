//
// FILE            migrateHistory.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The temporal history (TRoE) of a stream goes to the TRoE plugin as the EVENTS a live write
// produces (TroeDriver.h) - one per entity event, one per attribute instance - so the plugin's own
// code lays out its storage. An imported instance carries its instanceId and createdAt along
// (TroeEvent.instanceId / createdAtNs); modifiedAtNs is the instance's modifiedAt.
//
#include <stdio.h>                                        // fprintf
#include <string.h>                                       // strcmp, memset
#include <stdint.h>                                       // int64_t

#include "corAlloc/corAlloc.h"                            // corAlloc
#include "corTree/CorNode.h"                              // CorNode
#include "corTree/corTreeLookup.h"                        // corTreeLookup
#include "corTree/corTreeBuilder.h"                       // corTreeObject, corTreeChildAdd, corTreeString
#include "corTree/corTreeClone.h"                         // corTreeClone
#include "corRest/CorRestState.h"                         // corRest
#include "corJsonld/corLdExpandTree.h"                    // corLdExpandTree
#include "corJsonld/corLdInit.h"                          // corLdCoreContext

#include "corNgsild/LdVocab.h"                            // LD_VOCAB_*
#include "corNgsild/ldTermId.h"                           // ldTermId, CorTerm*
#include "corNgsild/ldIsEntityKeyword.h"                  // ldIsEntityMember

#include "troe/TroeDriver.h"                              // troe, TroeEvent, TroeOp*
#include "db/Tenant.h"                                    // Tenant

#include "migrate/MigrateState.h"                         // MigrateState
#include "migrate/migrateUtil.h"                          // migrateTermCheck, migrateTimeOf, ...
#include "migrate/migrateEntity.h"                        // migrateEntityToDbModel
#include "migrate/migrateHistory.h"                       // Own interface



// -----------------------------------------------------------------------------
//
// MIGRATE_HISTORY_BATCH - events per transaction
//
#define MIGRATE_HISTORY_BATCH 500



// -----------------------------------------------------------------------------
//
// troeCanImport - does the TRoE plugin take events at all?
//
// --troe corDB records its history at its own write sites and takes no events: importing history
// into it is not implemented yet (doc/migration.md).
//
static bool troeCanImport(MigrateState* msP, MigrateKind kind)
{
  if ((troe.alias == NULL) || (strcmp(troe.alias, "none") == 0))
    return migrateFail(msP, kind, "the broker runs without a TRoE plugin (--troe) - nowhere to put the history");

  if ((troe.eventList == NULL) && (troe.attrEvent == NULL))
    return migrateFail(msP, kind, "the TRoE plugin '%s' cannot import history yet", troe.alias);

  return true;
}



// -----------------------------------------------------------------------------
//
// migrateHistoryFlush - write the pending events, as one batch
//
bool migrateHistoryFlush(MigrateState* msP)
{
  if (msP->pendingN == 0)
    return true;

  int r = TROE_OK;

  if (troe.eventList != NULL)
    r = troe.eventList(msP->pendingHead, msP->pendingN);
  else
  {
    for (TroeEvent* evP = msP->pendingHead; (evP != NULL) && (r == TROE_OK); evP = evP->next)
    {
      bool entityOp = ((evP->op == TroeOpEntityCreated) || (evP->op == TroeOpEntityReplaced) || (evP->op == TroeOpEntityDeleted));

      if (entityOp == true)
        r = (troe.entityEvent != NULL) ? troe.entityEvent(evP) : TROE_ERR;
      else
        r = (troe.attrEvent   != NULL) ? troe.attrEvent(evP)   : TROE_ERR;
    }
  }

  bool ok = (r == TROE_OK);

  for (int kind = 0; kind < MigrateKinds; kind++)
  {
    if (ok == true)
      msP->okV[kind] += msP->pendingKindN[kind];
    else
      msP->failedV[kind] += msP->pendingKindN[kind];

    msP->pendingKindN[kind] = 0;
  }

  if (ok == false)
    fprintf(stderr, "%s:%d-%d: history: the batch of %d events was not written by the TRoE plugin '%s' (see its log)\n",
            msP->path, msP->pendingFirstLine, msP->lineNo, msP->pendingN, troe.alias);

  msP->pendingHead    = NULL;
  msP->pendingTail    = NULL;
  msP->pendingN       = 0;
  msP->pendingTenantP = NULL;

  return ok;
}



// -----------------------------------------------------------------------------
//
// pendingAdd - queue an event; a batch is ONE tenant (one connection, one transaction)
//
static void pendingAdd(MigrateState* msP, MigrateKind kind, TroeEvent* evP)
{
  if ((msP->pendingN > 0) && (msP->pendingTenantP != evP->tenantP))
    migrateHistoryFlush(msP);

  if (msP->pendingN == 0)
  {
    msP->pendingFirstLine = msP->lineNo;
    msP->pendingTenantP   = evP->tenantP;
    msP->pendingHead      = evP;
  }
  else
    msP->pendingTail->next = evP;

  msP->pendingTail = evP;
  msP->pendingN   += 1;
  msP->pendingKindN[kind] += 1;
}



// -----------------------------------------------------------------------------
//
// migrateHistoryBatchFull -
//
bool migrateHistoryBatchFull(MigrateState* msP)
{
  return (msP->pendingN >= MIGRATE_HISTORY_BATCH);
}



// -----------------------------------------------------------------------------
//
// opFromString -
//
static int opFromString(const char* op, bool entityLevel)
{
  if (op == NULL)
    return -1;

  if (entityLevel == true)
  {
    if (strcmp(op, "created")  == 0) return TroeOpEntityCreated;
    if (strcmp(op, "replaced") == 0) return TroeOpEntityReplaced;
    if (strcmp(op, "deleted")  == 0) return TroeOpEntityDeleted;
  }
  else
  {
    if (strcmp(op, "created")  == 0) return TroeOpAttrCreated;
    if (strcmp(op, "modified") == 0) return TroeOpAttrModified;
    if (strcmp(op, "replaced") == 0) return TroeOpAttrReplaced;
    if (strcmp(op, "deleted")  == 0) return TroeOpAttrDeleted;
  }

  return -1;
}



// -----------------------------------------------------------------------------
//
// typeExpand - the "type" member of a record, expanded: { "type": ... } as the plugins read it
//
static CorNode* typeExpand(MigrateState* msP, MigrateKind kind, CorNode* typeP, const char** firstTypeP)
{
  *firstTypeP = NULL;

  if (typeP->type == CorString)
  {
    if (migrateTermCheck(msP, kind, typeP->value.s, "entity type") == false)
      return NULL;
  }
  else if ((typeP->type == CorArray) && (typeP->value.head != NULL))
  {
    for (CorNode* tP = typeP->value.head; tP != NULL; tP = tP->next)
    {
      if (tP->type != CorString)
      {
        migrateFail(msP, kind, "an entity type is no string");
        return NULL;
      }

      if (migrateTermCheck(msP, kind, tP->value.s, "entity type") == false)
        return NULL;
    }
  }
  else
  {
    migrateFail(msP, kind, "the entity type is neither a string nor an array of strings");
    return NULL;
  }

  CorNode* holderP = corTreeObject(corRest.kallocP, NULL);
  CorNode* cloneP  = corTreeClone(corRest.kallocP, typeP);

  cloneP->name = (char*) "type";
  corTreeChildAdd(holderP, cloneP);
  corLdExpandTree(holderP, (msP->contextP != NULL) ? msP->contextP : corLdCoreContext(), &corRest.kalloc);

  CorNode* expandedP = corTreeLookup(holderP, "type");

  if (expandedP == NULL)
  {
    migrateFail(msP, kind, "the entity type did not expand");
    return NULL;
  }

  if (expandedP->type == CorString)
    *firstTypeP = expandedP->value.s;
  else if ((expandedP->type == CorArray) && (expandedP->value.head != NULL) && (expandedP->value.head->type == CorString))
    *firstTypeP = expandedP->value.head->value.s;

  return holderP;
}



// -----------------------------------------------------------------------------
//
// migrateTemporalEntity - one entity-level event: { id, type, op: created|replaced|deleted, at }
//
bool migrateTemporalEntity(MigrateState* msP, Tenant* tenantP, CorNode* dataP)
{
  if (troeCanImport(msP, MigrateTemporalEntity) == false)
    return false;

  if ((dataP == NULL) || (dataP->type != CorObject))
    return migrateFail(msP, MigrateTemporalEntity, "the record's data is no JSON object");

  CorNode*    idP = corTreeLookup(dataP, "id");
  CorNode*    opP = corTreeLookup(dataP, "op");
  int         op  = opFromString(((opP != NULL) && (opP->type == CorString)) ? opP->value.s : NULL, true);
  int64_t     at  = migrateTimeOf(corTreeLookup(dataP, "at"));

  if ((idP == NULL) || (idP->type != CorString))  return migrateFail(msP, MigrateTemporalEntity, "no entity id");
  if (op < 0)                                      return migrateFail(msP, MigrateTemporalEntity, "'%s': op must be created, replaced or deleted", idP->value.s);
  if (at <= 0)                                     return migrateFail(msP, MigrateTemporalEntity, "'%s': 'at' is no DateTime", idP->value.s);

  const char* firstType = NULL;
  CorNode*    typeHolderP = NULL;
  CorNode*    typeP       = corTreeLookup(dataP, "type");

  if (typeP != NULL)
  {
    typeHolderP = typeExpand(msP, MigrateTemporalEntity, typeP, &firstType);
    if (typeHolderP == NULL)
      return false;
  }
  else if (op != TroeOpEntityDeleted)
    return migrateFail(msP, MigrateTemporalEntity, "'%s': no type", idP->value.s);

  TroeEvent* evP = (TroeEvent*) corAlloc(&corRest.kalloc, sizeof(TroeEvent));
  memset(evP, 0, sizeof(TroeEvent));

  evP->op             = (TroeOp) op;
  evP->tenantP        = tenantP;
  evP->entityId       = idP->value.s;
  evP->entityType     = firstType;
  evP->modifiedAtNs   = (uint64_t) at;
  evP->entitySnapshot = typeHolderP;     // the type list only: an import's attribute rows come as records of their own

  pendingAdd(msP, MigrateTemporalEntity, evP);
  return true;
}



// -----------------------------------------------------------------------------
//
// migrateTemporalInstance - one attribute instance of the history
//
// { id, type, attr, op: created|modified|replaced|deleted, instance: {...} }
//
// The instance is an NGSI-LD attribute instance (normalized, names expanded) with its instanceId,
// createdAt, modifiedAt and, when it has one, datasetId. It is converted by the broker's own code
// (migrateEntityToDbModel, on a one-attribute entity), exactly as a live write's instance is.
//
bool migrateTemporalInstance(MigrateState* msP, Tenant* tenantP, CorNode* dataP)
{
  if (troeCanImport(msP, MigrateTemporalInstance) == false)
    return false;

  if ((dataP == NULL) || (dataP->type != CorObject))
    return migrateFail(msP, MigrateTemporalInstance, "the record's data is no JSON object");

  CorNode*  idP   = corTreeLookup(dataP, "id");
  CorNode*  attrP = corTreeLookup(dataP, "attr");
  CorNode*  opP   = corTreeLookup(dataP, "op");
  CorNode*  instP = corTreeLookup(dataP, "instance");
  int       op    = opFromString(((opP != NULL) && (opP->type == CorString)) ? opP->value.s : NULL, false);

  if ((idP == NULL)   || (idP->type   != CorString))  return migrateFail(msP, MigrateTemporalInstance, "no entity id");
  if ((attrP == NULL) || (attrP->type != CorString))  return migrateFail(msP, MigrateTemporalInstance, "'%s': no attr", idP->value.s);
  if (op < 0)                                         return migrateFail(msP, MigrateTemporalInstance, "'%s': op must be created, modified, replaced or deleted", idP->value.s);
  if ((instP == NULL) || (instP->type != CorObject))  return migrateFail(msP, MigrateTemporalInstance, "'%s': no instance", idP->value.s);

  if (migrateTermCheck(msP, MigrateTemporalInstance, attrP->value.s, "attribute name") == false)
    return false;

  const char* firstType   = NULL;
  CorNode*    typeP       = corTreeLookup(dataP, "type");

  if (typeP == NULL)
    return migrateFail(msP, MigrateTemporalInstance, "'%s': no entity type", idP->value.s);

  if (typeExpand(msP, MigrateTemporalInstance, typeP, &firstType) == NULL)
    return false;

  //
  // The instance's identity and system times are the event's, not the attribute's
  //
  const char* instanceId = migrateStringTake(instP, "instanceId");
  int64_t     createdAt  = migrateTimeTake(instP, CorTermCreatedAt,  LD_VOCAB_CREATED_AT);
  int64_t     modifiedAt = migrateTimeTake(instP, CorTermModifiedAt, LD_VOCAB_MODIFIED_AT);
  int64_t     deletedAt  = migrateTimeTake(instP, CorTermDeletedAt,  LD_VOCAB_DELETED_AT);

  if ((createdAt < 0) || (modifiedAt < 0) || (deletedAt < 0))
    return migrateFail(msP, MigrateTemporalInstance, "'%s' '%s': createdAt/modifiedAt/deletedAt is no DateTime", idP->value.s, attrP->value.s);

  if ((op == TroeOpAttrDeleted) && (deletedAt > 0))
    modifiedAt = deletedAt;

  if (modifiedAt == 0) modifiedAt = createdAt;
  if (modifiedAt == 0)
    return migrateFail(msP, MigrateTemporalInstance, "'%s' '%s': the instance has no modifiedAt", idP->value.s, attrP->value.s);
  if (createdAt == 0)  createdAt  = modifiedAt;

  CorNode*    wrapperP  = NULL;
  const char* datasetId = "";

  if (op == TroeOpAttrDeleted)
  {
    //
    // A deletion has no value to convert: the snapshot is the attribute's type alone, which is
    // what the plugin keeps of a deleted instance (its kind).
    //
    CorNode*    dsP     = corTreeLookup(instP, LD_VOCAB_DATASET_ID);
    CorNode*    atypeP  = corTreeLookup(instP, "type");
    CorNode*    holderP = corTreeObject(corRest.kallocP, NULL);
    CorNode*    emptyP  = corTreeObject(corRest.kallocP, attrP->value.s);

    corTreeChildAdd(holderP, emptyP);
    corLdExpandTree(holderP, (msP->contextP != NULL) ? msP->contextP : corLdCoreContext(), &corRest.kalloc);

    datasetId = ((dsP != NULL) && (dsP->type == CorString)) ? dsP->value.s : "";
    wrapperP  = corTreeObject(corRest.kallocP, holderP->value.head->name);

    CorNode* oneP = corTreeObject(corRest.kallocP, (datasetId[0] != 0) ? datasetId : "@none");
    if ((atypeP != NULL) && (atypeP->type == CorString))
      corTreeChildAdd(oneP, corTreeString(corRest.kallocP, "type", atypeP->value.s));
    corTreeChildAdd(wrapperP, oneP);
  }
  else
  {
    //
    // A one-attribute entity, through the same conversion as the current state
    //
    CorNode* entityP = corTreeObject(corRest.kallocP, NULL);
    CorNode* instCopyP = corTreeClone(corRest.kallocP, instP);

    instCopyP->name = attrP->value.s;
    corTreeChildAdd(entityP, corTreeString(corRest.kallocP, "id", idP->value.s));
    corTreeChildAdd(entityP, corTreeClone(corRest.kallocP, typeP));
    corTreeChildAdd(entityP, instCopyP);

    int64_t ec;
    int64_t em;

    if (migrateEntityToDbModel(msP, MigrateTemporalInstance, entityP, &ec, &em) == false)
      return false;

    for (CorNode* memberP = entityP->value.head; memberP != NULL; memberP = memberP->next)
    {
      if ((memberP->type == CorObject) && (memberP->name != NULL) && (ldIsEntityMember(memberP) == false))
      {
        wrapperP = memberP;
        break;
      }
    }

    if ((wrapperP == NULL) || (wrapperP->value.head == NULL))
      return migrateFail(msP, MigrateTemporalInstance, "'%s' '%s': the instance did not convert", idP->value.s, attrP->value.s);

    if (strcmp(wrapperP->value.head->name, "@none") != 0)
      datasetId = wrapperP->value.head->name;

    //
    // The instance's own times, on the converted instance too (the plugin may read them there)
    //
    CorNode* cP = corTreeLookup(wrapperP->value.head, LD_VOCAB_CREATED_AT);
    CorNode* mP = corTreeLookup(wrapperP->value.head, LD_VOCAB_MODIFIED_AT);
    if ((cP != NULL) && (cP->type == CorInt)) cP->value.i = (long long) createdAt;
    if ((mP != NULL) && (mP->type == CorInt)) mP->value.i = (long long) modifiedAt;
  }

  TroeEvent* evP = (TroeEvent*) corAlloc(&corRest.kalloc, sizeof(TroeEvent));
  memset(evP, 0, sizeof(TroeEvent));

  evP->op           = (TroeOp) op;
  evP->tenantP      = tenantP;
  evP->entityId     = idP->value.s;
  evP->entityType   = firstType;
  evP->attrName     = wrapperP->name;
  evP->datasetId    = datasetId;
  evP->attrSnapshot = wrapperP;
  evP->modifiedAtNs = (uint64_t) modifiedAt;
  evP->createdAtNs  = (uint64_t) createdAt;
  evP->instanceId   = instanceId;

  pendingAdd(msP, MigrateTemporalInstance, evP);
  return true;
}
