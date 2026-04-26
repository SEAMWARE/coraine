//
// FILE            ldLinkedEntities.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// NGSI-LD § 4.5.23 — linked-entity retrieval (flat representation).
//
#include <stdlib.h>                                   // calloc, free
#include <string.h>                                   // strcmp, strdup

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjBuilder.h"                          // kjArray, kjChildAdd

#include "swRest/SwRestState.h"                       // swRest

#include "swNgsild/LdVocab.h"                         // LD_VOCAB_HAS_OBJECT

#include "db/DbDriver.h"                              // db, DB_OK
#include "db/Tenant.h"                                // Tenant

#include "linkedEntities/ldLinkedEntities.h"          // Own interface



// -----------------------------------------------------------------------------
//
// VisitedNode - linked list of entity ids already added to the result
//
typedef struct VisitedNode
{
  const char*         id;       // borrowed pointer into the entity tree
  struct VisitedNode* next;
} VisitedNode;



static bool visitedContains(VisitedNode* head, const char* id)
{
  for (VisitedNode* n = head; n != NULL; n = n->next)
    if (n->id != NULL && strcmp(n->id, id) == 0)
      return true;
  return false;
}



static VisitedNode* visitedAppend(VisitedNode* head, const char* id)
{
  VisitedNode* n = (VisitedNode*) calloc(1, sizeof(VisitedNode));
  n->id   = id;
  n->next = head;
  return n;
}



static void visitedFree(VisitedNode* head)
{
  while (head != NULL)
  {
    VisitedNode* next = head->next;
    free(head);
    head = next;
  }
}



// -----------------------------------------------------------------------------
//
// entityIdOf - extract the "id" of an entity tree (borrowed pointer)
//
static const char* entityIdOf(KjNode* entityP)
{
  if (entityP == NULL || entityP->type != KjObject)
    return NULL;

  KjNode* idP = kjLookup(entityP, "id");
  if (idP == NULL || idP->type != KjString)
    return NULL;

  return idP->value.s;
}



// -----------------------------------------------------------------------------
//
// collectRelationshipTargets - append target ids of every Relationship in entityP
//
// Storage layout (post-DB fetch, after ldApiEntityToDbModel):
//   entityP ─ <attr-iri> ─ <datasetId>  ─ type:  "Relationship"
//                                         value: "<uri>"
//
// Note: HAS_OBJECT is renamed to "value" by ldApiEntityToDbModel's
// normalizeValueKey at write time, so we read "value" here. The
// instance's type field disambiguates Relationship from Property
// (which also stores a value).
//
// Top-level entity-level keys (id, type, createdAt, modifiedAt, scope)
// are skipped. attribute containers are KjObjects; their children are
// instance objects keyed by datasetId (or "@none" for the default).
//
static void collectRelationshipTargets(KjNode* entityP, const char*** outIdsP, int* outCountP, int* outCapP)
{
  for (KjNode* attrP = entityP->value.firstChildP; attrP != NULL; attrP = attrP->next)
  {
    if (attrP->name == NULL)
      continue;
    if (attrP->name[0] == '@')                                    continue;
    if (attrP->type != KjObject)                                  continue;
    if (strcmp(attrP->name, "id")         == 0)                   continue;
    if (strcmp(attrP->name, "type")       == 0)                   continue;
    if (strcmp(attrP->name, "createdAt")  == 0)                   continue;
    if (strcmp(attrP->name, "modifiedAt") == 0)                   continue;
    if (strcmp(attrP->name, "scope")      == 0)                   continue;

    for (KjNode* instP = attrP->value.firstChildP; instP != NULL; instP = instP->next)
    {
      if (instP->type != KjObject)
        continue;

      KjNode* typeP = kjLookup(instP, "type");
      if (typeP == NULL || typeP->type != KjString)               continue;
      if (strcmp(typeP->value.s, "Relationship") != 0)            continue;

      KjNode* valP = kjLookup(instP, "value");
      if (valP == NULL || valP->type != KjString || valP->value.s == NULL)
        continue;

      // Append (grow array if needed)
      if (*outCountP >= *outCapP)
      {
        int newCap = (*outCapP == 0) ? 8 : (*outCapP) * 2;
        const char** newArr = (const char**) realloc((void*) *outIdsP, newCap * sizeof(char*));
        *outIdsP = newArr;
        *outCapP = newCap;
      }
      (*outIdsP)[(*outCountP)++] = valP->value.s;
    }
  }
}



// -----------------------------------------------------------------------------
//
// flatBfs - BFS body shared by single-primary and array expanders
//
// Seeded with `frontier`, BFS up to `joinLevel` hops; each newly-fetched
// target is appended to `outArr` and tracked in **visitedPP. Caller owns
// the initial frontier malloc; the helper frees it (and the per-level
// next-frontier).
//
static void flatBfs(KjNode*          outArr,
                    KjNode**         frontier,
                    int              frontierCount,
                    int              joinLevel,
                    Tenant*          tenantP,
                    VisitedNode**    visitedPP)
{
  if (frontier == NULL || frontierCount <= 0)
    return;
  if (joinLevel < 1 || tenantP == NULL || db.entityRetrieve == NULL)
  {
    free(frontier);
    return;
  }

  for (int depth = 0; depth < joinLevel; depth++)
  {
    const char** targets   = NULL;
    int          targetN   = 0;
    int          targetCap = 0;

    for (int i = 0; i < frontierCount; i++)
      collectRelationshipTargets(frontier[i], &targets, &targetN, &targetCap);

    KjNode** nextFrontier      = NULL;
    int      nextFrontierCount = 0;
    int      nextFrontierCap   = 0;

    for (int t = 0; t < targetN; t++)
    {
      const char* tid = targets[t];

      if (visitedContains(*visitedPP, tid))
        continue;

      KjNode* targetEntityP = NULL;
      int     r             = db.entityRetrieve(tenantP, tid, &targetEntityP);
      if (r != DB_OK || targetEntityP == NULL)
      {
        *visitedPP = visitedAppend(*visitedPP, tid);
        continue;
      }

      kjChildAdd(outArr, targetEntityP);
      *visitedPP = visitedAppend(*visitedPP, entityIdOf(targetEntityP));

      if (nextFrontierCount >= nextFrontierCap)
      {
        nextFrontierCap = (nextFrontierCap == 0) ? 8 : nextFrontierCap * 2;
        nextFrontier    = (KjNode**) realloc(nextFrontier, nextFrontierCap * sizeof(KjNode*));
      }
      nextFrontier[nextFrontierCount++] = targetEntityP;
    }

    free(targets);
    free(frontier);
    frontier      = nextFrontier;
    frontierCount = nextFrontierCount;

    if (frontierCount == 0)
      break;
  }

  free(frontier);
}



// -----------------------------------------------------------------------------
//
// ldLinkedEntitiesFlat -
//
KjNode* ldLinkedEntitiesFlat(KjNode* primaryP, int joinLevel, Tenant* tenantP)
{
  KjNode* outArr = kjArray(swRest.kjsonP, NULL);
  if (primaryP == NULL)
    return outArr;

  const char* primaryId = entityIdOf(primaryP);
  if (primaryId == NULL)
  {
    kjChildAdd(outArr, primaryP);
    return outArr;
  }

  kjChildAdd(outArr, primaryP);

  VisitedNode* visited     = visitedAppend(NULL, primaryId);
  KjNode**     frontier    = (KjNode**) malloc(sizeof(KjNode*));
  frontier[0]              = primaryP;
  flatBfs(outArr, frontier, 1, joinLevel, tenantP, &visited);

  visitedFree(visited);
  return outArr;
}



// -----------------------------------------------------------------------------
//
// ldLinkedEntitiesExpandArrayFlat -
//
void ldLinkedEntitiesExpandArrayFlat(KjNode* arrayP, int joinLevel, Tenant* tenantP)
{
  if (arrayP == NULL || arrayP->type != KjArray)
    return;
  if (arrayP->value.firstChildP == NULL)
    return;

  // Count primaries + seed visited-set with their ids
  VisitedNode* visited      = NULL;
  int          primaryCount = 0;
  for (KjNode* eP = arrayP->value.firstChildP; eP != NULL; eP = eP->next)
  {
    const char* id = entityIdOf(eP);
    if (id != NULL)
      visited = visitedAppend(visited, id);
    primaryCount++;
  }

  // Build a frontier snapshot (kjChildAdd inside the BFS would otherwise
  // mutate the live ->next chain we're iterating).
  KjNode** frontier = (KjNode**) malloc(primaryCount * sizeof(KjNode*));
  int      idx      = 0;
  for (KjNode* eP = arrayP->value.firstChildP; eP != NULL && idx < primaryCount; eP = eP->next)
    frontier[idx++] = eP;

  flatBfs(arrayP, frontier, primaryCount, joinLevel, tenantP, &visited);

  visitedFree(visited);
}



// -----------------------------------------------------------------------------
//
// ldLinkedEntitiesExpandArrayInline - inline-expand each primary in arrayP
//
// Each primary gets its own per-primary visited-set: a target shared by
// two primaries is correctly inlined under both. (For flat the spec
// dedupes globally; inline is per-linking-entity by design.)
//
void ldLinkedEntitiesExpandArrayInline(KjNode* arrayP, int joinLevel, Tenant* tenantP)
{
  if (arrayP == NULL || arrayP->type != KjArray)
    return;

  for (KjNode* eP = arrayP->value.firstChildP; eP != NULL; eP = eP->next)
    ldLinkedEntitiesInline(eP, joinLevel, tenantP);
}



#include "swNgsild/ldEntityToApi.h"                   // ldEntityToApi



// -----------------------------------------------------------------------------
//
// inlineWalk - recursive depth-bounded traversal for the inline shape
//
// Post-order: descend storage targets first (so the storage walker can
// find their relationships), then call ldEntityToApi on each child
// before attaching to its parent. The primary stays in storage; the
// service routine's renderHook converts it.
//
static void inlineWalk(KjNode*       entityP,
                       int           remaining,
                       VisitedNode** visitedPP,
                       Tenant*       tenantP)
{
  if (entityP == NULL || remaining <= 0)
    return;

  for (KjNode* attrP = entityP->value.firstChildP; attrP != NULL; attrP = attrP->next)
  {
    if (attrP->name == NULL)                                      continue;
    if (attrP->name[0] == '@')                                    continue;
    if (attrP->type != KjObject)                                  continue;
    if (strcmp(attrP->name, "id")         == 0)                   continue;
    if (strcmp(attrP->name, "type")       == 0)                   continue;
    if (strcmp(attrP->name, "createdAt")  == 0)                   continue;
    if (strcmp(attrP->name, "modifiedAt") == 0)                   continue;
    if (strcmp(attrP->name, "scope")      == 0)                   continue;

    for (KjNode* instP = attrP->value.firstChildP; instP != NULL; instP = instP->next)
    {
      if (instP->type != KjObject)
        continue;

      KjNode* typeP = kjLookup(instP, "type");
      if (typeP == NULL || typeP->type != KjString)               continue;
      if (strcmp(typeP->value.s, "Relationship") != 0)            continue;

      KjNode* valP = kjLookup(instP, "value");
      if (valP == NULL || valP->type != KjString)                 continue;

      const char* tid = valP->value.s;

      if (visitedContains(*visitedPP, tid))
        continue;

      KjNode* targetP = NULL;
      int     r       = db.entityRetrieve(tenantP, tid, &targetP);
      if (r != DB_OK || targetP == NULL)
      {
        *visitedPP = visitedAppend(*visitedPP, tid);
        continue;
      }

      *visitedPP = visitedAppend(*visitedPP, entityIdOf(targetP));

      // Recurse while target is still in storage so the storage
      // walker can find its Relationships.
      inlineWalk(targetP, remaining - 1, visitedPP, tenantP);

      // Convert the target itself to API (post-order). Its inlined
      // children are already API-converted from the recursion above.
      ldEntityToApi(targetP, &swRest.kalloc);

      // Attach to the originating Relationship instance as "entity".
      targetP->name = "entity";
      kjChildAdd(instP, targetP);
    }
  }
}



// -----------------------------------------------------------------------------
//
// ldLinkedEntitiesInline -
//
KjNode* ldLinkedEntitiesInline(KjNode* primaryP, int joinLevel, Tenant* tenantP)
{
  if (primaryP == NULL)
    return NULL;

  const char* primaryId = entityIdOf(primaryP);
  if (primaryId == NULL || joinLevel < 1 || tenantP == NULL || db.entityRetrieve == NULL)
    return primaryP;

  VisitedNode* visited = visitedAppend(NULL, primaryId);
  inlineWalk(primaryP, joinLevel, &visited, tenantP);
  visitedFree(visited);

  return primaryP;
}
