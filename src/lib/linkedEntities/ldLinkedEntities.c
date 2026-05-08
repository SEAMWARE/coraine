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
#include <string.h>                                   // strcmp, strdup, memcpy

#include "kalloc/kaAlloc.h"                           // kaAlloc

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjBuilder.h"                          // kjArray, kjChildAdd
#include "kjson/kjParse.h"                            // kjParse

#include "swRest/SwRestState.h"                       // swRest
#include "swRest/SwRestVerb.h"                        // SwVerbGet

#include "swNgsild/LdVocab.h"                         // LD_VOCAB_HAS_OBJECT
#include "swNgsild/SwNgsild.h"                        // ldLocalOnly, swNgsild
#include "swNgsild/LdProj.h"                          // LdProjItem, ldProjectionFindChild, ldProjectionTopLevelNames
#include "swNgsild/ldPickOmit.h"                      // ldPickOmit
#include "swNgsild/LdRegCache.h"                      // LdRegCache, LdRegCacheItem, LdRegMode
#include "swNgsild/ldRegCache.h"                      // ldRegCacheMatchForRetrieve
#include "swNgsild/ldDistOp.h"                        // ldDistOpSendReceive, ldDistOpCsrWouldLoop
#include "swNgsild/ldCsourceAlias.h"                  // ldCsourceAliasForTenant
#include "swNgsild/ldApiEntityToDbModel.h"            // ldApiEntityToDbModel
#include "swNgsild/ldStripAtContext.h"                // ldStripAtContext

#include "swJsonld/swldExpandTree.h"                  // swldExpandTree

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



//
// visitedSeedFromContainedBy - pre-populate the visited set with the
// entity ids that came in via ?containedBy= (§ 5.7.1.4 cycle prevention
// for linked-entity retrieval). Returns the new head — or the input
// head when ?containedBy is absent. Callers must visitedFree() the
// returned head.
//
static VisitedNode* visitedSeedFromContainedBy(VisitedNode* head)
{
  if (swNgsild.containedByV == NULL) return head;
  for (int i = 0; swNgsild.containedByV[i] != NULL; i++)
    head = visitedAppend(head, swNgsild.containedByV[i]);
  return head;
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
// linkedFetchOne - § 4.5.23 entity lookup, local DB first, then dist-op
//
// Tries db.entityRetrieve in storage shape first. On miss, walks the
// tenant's reg-cache for any registration covering this id (across all
// modes), forwards GET /entities/{id} via ldDistOpSendReceive, parses
// the API-shape response, and converts to storage via
// ldApiEntityToDbModel so the caller (walker / q-evaluator) sees one
// consistent shape.
//
// Returns 0 on success and *entityPP set; non-zero on miss.
//
int linkedFetchOne(const char* entityId, KjNode** entityPP, Tenant* tenantP)
{
  if (entityId == NULL || entityPP == NULL || tenantP == NULL)
    return -1;

  *entityPP = NULL;

  if (db.entityRetrieve != NULL)
  {
    int r = db.entityRetrieve(tenantP, entityId, entityPP);
    if (r == DB_OK && *entityPP != NULL)
      return 0;
  }

  if (ldLocalOnly || tenantP->regCacheP == NULL)
    return -1;

  LdRegCache* regCacheP = (LdRegCache*) tenantP->regCacheP;
  const char* ownAlias  = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);

  static const LdRegMode modes[4] = { LdRegModeExclusive, LdRegModeRedirect,
                                       LdRegModeInclusive, LdRegModeAuxiliary };

  for (int m = 0; m < 4; m++)
  {
    LdRegCacheItem** matchV = NULL;
    int matchN = ldRegCacheMatchForRetrieve(regCacheP, entityId, NULL, modes[m], &matchV);

    for (int i = 0; i < matchN; i++)
    {
      LdRegCacheItem* csr = matchV[i];
      if (csr->endpoint == NULL)                continue;
      if (ldDistOpCsrWouldLoop(csr, ownAlias))  continue;

      // Build "<endpoint>/ngsi-ld/v1/entities/<id>"
      static const char* path = "/ngsi-ld/v1/entities/";
      int   epLen   = (int) strlen(csr->endpoint);
      int   pathLen = (int) strlen(path);
      int   idLen   = (int) strlen(entityId);
      char* url     = (char*) kaAlloc(&swRest.kalloc, epLen + pathLen + idLen + 1);
      char* p       = url;
      memcpy(p, csr->endpoint, epLen);  p += epLen;
      memcpy(p, path,          pathLen); p += pathLen;
      memcpy(p, entityId,      idLen + 1);

      const char* errDetail = NULL;
      char*       respBody  = NULL;
      int         respLen   = 0;

      int status = ldDistOpSendReceive(csr, SwVerbGet, url, NULL, 0, ownAlias,
                                       &errDetail, &respBody, &respLen);
      if (status == 200 && respBody != NULL && respLen > 0)
      {
        KjNode* tree = kjParse(swRest.kjsonP, respBody);
        if (tree != NULL && tree->type == KjObject)
        {
          // Remote payload arrives in API shape with short names. Expand
          // attr names to IRIs (so q-evaluator's IRI lookups hit) and
          // drop @context, then API → storage so the walker / q-evaluator
          // reads (datasetId wrappers, "value" instead of "object") line
          // up with the local-fetched shape.
          swldExpandTree(tree, swNgsild.contextP, &swRest.kalloc);
          ldStripAtContext(tree);
          ldApiEntityToDbModel(tree, &swRest.kalloc);
          *entityPP = tree;
          if (matchV != NULL) free(matchV);
          return 0;
        }
      }
    }

    if (matchV != NULL) free(matchV);
  }

  return -1;
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
// Flat-BFS frontier carries per-entity pick/omit sub-trees so the
// projection's `parent{child1,child2}` clause applies to entities reached
// via that parent relationship — and recurses into deeper {…} clauses
// across BFS levels.
typedef struct FlatFrontier
{
  KjNode*      entityP;
  LdProjItem*  pickSub;
  LdProjItem*  omitSub;
} FlatFrontier;


static void flatBfs(KjNode*         outArr,
                    FlatFrontier*   frontier,
                    int             frontierCount,
                    int             joinLevel,
                    Tenant*         tenantP,
                    VisitedNode**   visitedPP)
{
  if (frontier == NULL || frontierCount <= 0)
    return;
  if (joinLevel < 1 || tenantP == NULL)
  {
    free(frontier);
    return;
  }

  for (int depth = 0; depth < joinLevel; depth++)
  {
    FlatFrontier* nextFrontier      = NULL;
    int           nextFrontierCount = 0;
    int           nextFrontierCap   = 0;

    for (int i = 0; i < frontierCount; i++)
    {
      KjNode*     fromP    = frontier[i].entityP;
      LdProjItem* fromPick = frontier[i].pickSub;
      LdProjItem* fromOmit = frontier[i].omitSub;

      // Walk the from-entity's relationship attributes, fetching each
      // target with the projection sub-tree determined by the from-side
      // pick/omit's child for THIS attribute name.
      for (KjNode* attrP = fromP->value.firstChildP; attrP != NULL; attrP = attrP->next)
      {
        if (attrP->name == NULL)                                      continue;
        if (attrP->name[0] == '@')                                    continue;
        if (attrP->type != KjObject)                                  continue;
        if (strcmp(attrP->name, "id")         == 0)                   continue;
        if (strcmp(attrP->name, "type")       == 0)                   continue;
        if (strcmp(attrP->name, "createdAt")  == 0)                   continue;
        if (strcmp(attrP->name, "modifiedAt") == 0)                   continue;
        if (strcmp(attrP->name, "scope")      == 0)                   continue;

        LdProjItem* subPick = ldProjectionFindChild(fromPick, attrP->name);
        LdProjItem* subOmit = ldProjectionFindChild(fromOmit, attrP->name);

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

          const char* tid = valP->value.s;
          if (visitedContains(*visitedPP, tid))
            continue;

          KjNode* targetEntityP = NULL;
          int     r             = linkedFetchOne(tid, &targetEntityP, tenantP);
          if (r != 0 || targetEntityP == NULL)
          {
            *visitedPP = visitedAppend(*visitedPP, tid);
            continue;
          }

          // Apply pick/omit on the linked entity in storage form (the
          // projection trees carry expanded IRIs). renderHook converts
          // to API form afterwards.
          if (subPick != NULL || subOmit != NULL)
          {
            char** subPickV = (subPick != NULL)
                              ? ldProjectionTopLevelNames(subPick, &swRest.kalloc, true)
                              : NULL;
            char** subOmitV = (subOmit != NULL)
                              ? ldProjectionTopLevelNames(subOmit, &swRest.kalloc, false)
                              : NULL;
            ldPickOmit(targetEntityP, subPickV, subOmitV);
          }

          kjChildAdd(outArr, targetEntityP);
          *visitedPP = visitedAppend(*visitedPP, entityIdOf(targetEntityP));

          if (nextFrontierCount >= nextFrontierCap)
          {
            nextFrontierCap = (nextFrontierCap == 0) ? 8 : nextFrontierCap * 2;
            nextFrontier    = (FlatFrontier*) realloc(nextFrontier, nextFrontierCap * sizeof(FlatFrontier));
          }
          nextFrontier[nextFrontierCount].entityP = targetEntityP;
          nextFrontier[nextFrontierCount].pickSub = subPick;
          nextFrontier[nextFrontierCount].omitSub = subOmit;
          nextFrontierCount++;
        }
      }
    }

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

  VisitedNode*   visited  = visitedAppend(NULL, primaryId);
  visited                 = visitedSeedFromContainedBy(visited);
  FlatFrontier*  frontier = (FlatFrontier*) malloc(sizeof(FlatFrontier));
  frontier[0].entityP     = primaryP;
  frontier[0].pickSub     = swNgsild.pickTree;
  frontier[0].omitSub     = swNgsild.omitTree;
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
  visited = visitedSeedFromContainedBy(visited);

  // Build a frontier snapshot (kjChildAdd inside the BFS would otherwise
  // mutate the live ->next chain we're iterating). Each primary inherits
  // the request's top-level pick/omit sub-trees as its starting point.
  FlatFrontier* frontier = (FlatFrontier*) malloc(primaryCount * sizeof(FlatFrontier));
  int           idx      = 0;
  for (KjNode* eP = arrayP->value.firstChildP; eP != NULL && idx < primaryCount; eP = eP->next)
  {
    frontier[idx].entityP = eP;
    frontier[idx].pickSub = swNgsild.pickTree;
    frontier[idx].omitSub = swNgsild.omitTree;
    idx++;
  }

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
                       Tenant*       tenantP,
                       LdProjItem*   pickTree,
                       LdProjItem*   omitTree)
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

    // § 4.21 / § 4.5.23 attribute projection: when pick/omit is
    // `parent{child1,child2}`, the {…} sub-projection applies to the
    // linked entity reached through `parent`. Compute the per-relationship
    // sub-trees here once; both inline-recursion and fetch-time
    // pick/omit at this level use them.
    LdProjItem* subPick = ldProjectionFindChild(pickTree, attrP->name);
    LdProjItem* subOmit = ldProjectionFindChild(omitTree, attrP->name);

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
      int     r       = linkedFetchOne(tid, &targetP, tenantP);
      if (r != 0 || targetP == NULL)
      {
        *visitedPP = visitedAppend(*visitedPP, tid);
        continue;
      }

      *visitedPP = visitedAppend(*visitedPP, entityIdOf(targetP));

      // Recurse while target is still in storage so the storage
      // walker can find its Relationships. The target's pick/omit
      // sub-trees become the "current level" for one more step.
      inlineWalk(targetP, remaining - 1, visitedPP, tenantP, subPick, subOmit);

      // Apply pick/omit to the linked entity BEFORE API-conversion —
      // the parsed projection trees carry expanded IRIs, matching the
      // storage form of attribute names. Top-level pick/omit on the
      // primary entity is applied by the service routine; this is the
      // analogous step for each linked level.
      if (subPick != NULL || subOmit != NULL)
      {
        char** subPickV = (subPick != NULL)
                          ? ldProjectionTopLevelNames(subPick, &swRest.kalloc, true)
                          : NULL;
        char** subOmitV = (subOmit != NULL)
                          ? ldProjectionTopLevelNames(subOmit, &swRest.kalloc, false)
                          : NULL;
        ldPickOmit(targetP, subPickV, subOmitV);
      }

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
  if (primaryId == NULL || joinLevel < 1 || tenantP == NULL)
    return primaryP;

  VisitedNode* visited = visitedAppend(NULL, primaryId);
  visited              = visitedSeedFromContainedBy(visited);
  inlineWalk(primaryP, joinLevel, &visited, tenantP, swNgsild.pickTree, swNgsild.omitTree);
  visitedFree(visited);

  return primaryP;
}



// -----------------------------------------------------------------------------
//
// collectRelationshipTargetsApi - API-format equivalent of collectRelationshipTargets
//
// API shape per attribute child:
//   - single instance:  { "type": "Relationship", "object": "<uri>", ... }
//   - multi-instance:   [ { "type": "Relationship", "object": "<uri>", ... }, ... ]
//
static void collectRelationshipTargetsApi(KjNode* entityP, const char*** outIdsP, int* outCountP, int* outCapP)
{
  for (KjNode* attrP = entityP->value.firstChildP; attrP != NULL; attrP = attrP->next)
  {
    if (attrP->name == NULL || attrP->name[0] == '@')               continue;
    if (strcmp(attrP->name, "id")         == 0)                     continue;
    if (strcmp(attrP->name, "type")       == 0)                     continue;
    if (strcmp(attrP->name, "createdAt")  == 0)                     continue;
    if (strcmp(attrP->name, "modifiedAt") == 0)                     continue;
    if (strcmp(attrP->name, "scope")      == 0)                     continue;

    KjNode* instances[16];
    int     instCount = 0;

    if (attrP->type == KjObject)
      instances[instCount++] = attrP;
    else if (attrP->type == KjArray)
    {
      for (KjNode* iP = attrP->value.firstChildP; iP != NULL && instCount < 16; iP = iP->next)
        if (iP->type == KjObject)
          instances[instCount++] = iP;
    }
    else
      continue;

    for (int i = 0; i < instCount; i++)
    {
      KjNode* typeP = kjLookup(instances[i], "type");
      if (typeP == NULL || typeP->type != KjString)                 continue;
      if (strcmp(typeP->value.s, "Relationship") != 0)              continue;

      KjNode* objP = kjLookup(instances[i], "object");
      if (objP == NULL || objP->type != KjString || objP->value.s == NULL) continue;

      if (*outCountP >= *outCapP)
      {
        int newCap = (*outCapP == 0) ? 8 : (*outCapP) * 2;
        const char** newArr = (const char**) realloc((void*) *outIdsP, newCap * sizeof(char*));
        *outIdsP = newArr;
        *outCapP = newCap;
      }
      (*outIdsP)[(*outCountP)++] = objP->value.s;
    }
  }
}



// -----------------------------------------------------------------------------
//
// notifFlatBfs - § 4.5.23.3 BFS for the API-format notification path
//
static void notifFlatBfs(KjNode* outArr, KjNode** frontier, int frontierCount,
                         int joinLevel, Tenant* tenantP, VisitedNode** visitedPP)
{
  if (frontier == NULL || frontierCount <= 0) return;
  if (joinLevel < 1 || tenantP == NULL) { free(frontier); return; }

  for (int depth = 0; depth < joinLevel; depth++)
  {
    const char** targets = NULL;
    int          targetN = 0;
    int          targetCap = 0;

    for (int i = 0; i < frontierCount; i++)
      collectRelationshipTargetsApi(frontier[i], &targets, &targetN, &targetCap);

    KjNode** nextFrontier      = NULL;
    int      nextFrontierCount = 0;
    int      nextFrontierCap   = 0;

    for (int t = 0; t < targetN; t++)
    {
      const char* tid = targets[t];
      if (visitedContains(*visitedPP, tid)) continue;

      KjNode* targetEntityP = NULL;
      int     r             = linkedFetchOne(tid, &targetEntityP, tenantP);
      if (r != 0 || targetEntityP == NULL)
      {
        *visitedPP = visitedAppend(*visitedPP, tid);
        continue;
      }

      // linkedFetchOne returns storage shape — convert to API to match
      // the existing entries in the notification's data[] array.
      ldEntityToApi(targetEntityP, &swRest.kalloc);

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
    if (frontierCount == 0) break;
  }
  free(frontier);
}



// -----------------------------------------------------------------------------
//
// notifInlineWalk - § 4.5.23.2 inline walk on API-format primary
//
static void notifInlineWalk(KjNode* primaryP, int joinLevel, VisitedNode** visitedPP, Tenant* tenantP)
{
  if (joinLevel < 1) return;

  for (KjNode* attrP = primaryP->value.firstChildP; attrP != NULL; attrP = attrP->next)
  {
    if (attrP->name == NULL || attrP->name[0] == '@')               continue;
    if (strcmp(attrP->name, "id")         == 0)                     continue;
    if (strcmp(attrP->name, "type")       == 0)                     continue;
    if (strcmp(attrP->name, "createdAt")  == 0)                     continue;
    if (strcmp(attrP->name, "modifiedAt") == 0)                     continue;
    if (strcmp(attrP->name, "scope")      == 0)                     continue;

    KjNode* instances[16];
    int     instCount = 0;
    if (attrP->type == KjObject)
      instances[instCount++] = attrP;
    else if (attrP->type == KjArray)
    {
      for (KjNode* iP = attrP->value.firstChildP; iP != NULL && instCount < 16; iP = iP->next)
        if (iP->type == KjObject)
          instances[instCount++] = iP;
    }
    else
      continue;

    for (int i = 0; i < instCount; i++)
    {
      KjNode* typeP = kjLookup(instances[i], "type");
      if (typeP == NULL || typeP->type != KjString) continue;
      if (strcmp(typeP->value.s, "Relationship") != 0) continue;

      KjNode* objP = kjLookup(instances[i], "object");
      if (objP == NULL || objP->type != KjString || objP->value.s == NULL) continue;

      const char* tid = objP->value.s;
      if (visitedContains(*visitedPP, tid)) continue;

      KjNode* targetEntityP = NULL;
      int     r             = linkedFetchOne(tid, &targetEntityP, tenantP);
      if (r != 0 || targetEntityP == NULL)
      {
        *visitedPP = visitedAppend(*visitedPP, tid);
        continue;
      }

      ldEntityToApi(targetEntityP, &swRest.kalloc);
      *visitedPP  = visitedAppend(*visitedPP, entityIdOf(targetEntityP));
      targetEntityP->name = (char*) "entity";
      kjChildAdd(instances[i], targetEntityP);

      if (joinLevel > 1)
        notifInlineWalk(targetEntityP, joinLevel - 1, visitedPP, tenantP);
    }
  }
}



// -----------------------------------------------------------------------------
//
// ldLinkedEntitiesNotifApiArray -
//
void ldLinkedEntitiesNotifApiArray(KjNode* arrayP, const char* mode, int joinLevel, Tenant* tenantP)
{
  if (arrayP == NULL || arrayP->type != KjArray || mode == NULL || tenantP == NULL) return;
  if (arrayP->value.firstChildP == NULL) return;

  if (strcmp(mode, "flat") == 0)
  {
    VisitedNode* visited      = NULL;
    int          primaryCount = 0;
    for (KjNode* eP = arrayP->value.firstChildP; eP != NULL; eP = eP->next)
    {
      const char* id = entityIdOf(eP);
      if (id != NULL) visited = visitedAppend(visited, id);
      primaryCount++;
    }

    KjNode** frontier = (KjNode**) malloc(primaryCount * sizeof(KjNode*));
    int      idx      = 0;
    for (KjNode* eP = arrayP->value.firstChildP; eP != NULL && idx < primaryCount; eP = eP->next)
      frontier[idx++] = eP;

    notifFlatBfs(arrayP, frontier, primaryCount, joinLevel, tenantP, &visited);
    visitedFree(visited);
  }
  else if (strcmp(mode, "inline") == 0)
  {
    for (KjNode* eP = arrayP->value.firstChildP; eP != NULL; eP = eP->next)
    {
      const char* id = entityIdOf(eP);
      if (id == NULL) continue;
      VisitedNode* visited = visitedAppend(NULL, id);
      notifInlineWalk(eP, joinLevel, &visited, tenantP);
      visitedFree(visited);
    }
  }
}
