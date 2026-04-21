//
// FILE            mongocEntityBulkMerge.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// mongoc Batch Merge: single pool checkout + single collection handle
// across the whole fragments array. Each fragment is applied via the
// factored mongocEntityMergeOne helper.
//
// v1 tradeoff: 2 round-trips per fragment (retrieve + surgical
// update_one) over one shared TCP connection. A future refactor will
// split mongocEntityMergeOne into compute-doc + execute-doc phases and
// submit a single mongoc_bulk_operation_execute for all updates.
//

#include <stddef.h>                                      // NULL

#include <mongoc/mongoc.h>                               // mongoc_client_*, mongoc_collection_*

#include "kjson/KjNode.h"                                // KjNode
#include "kjson/kjLookup.h"                              // kjLookup

#include "swNgsild/ldEntityMerge.h"                      // LdMergeReport

#include "db/DbDriver.h"                                 // DB_OK, DB_ERR, Tenant
#include "currentState/mongoc/mongocEntityMerge.h"       // mongocEntityMergeOne
#include "currentState/mongoc/mongocEntityBulkMerge.h"   // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocEntityBulkMerge -
//
int mongocEntityBulkMerge(Tenant* tenantP, KjNode* fragmentsArr,
                          uint64_t ts, int* resultsV,
                          LdMergeReport* reportsV,
                          KjNode** snapshotsV)
{
  if (fragmentsArr == NULL || fragmentsArr->type != KjArray)
    return DB_ERR;

  mongoc_client_t*     clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t* collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  int  ix    = 0;
  bool anyOk = false;

  for (KjNode* fragP = fragmentsArr->value.firstChildP; fragP != NULL; fragP = fragP->next, ix++)
  {
    KjNode* idP = kjLookup(fragP, "id");
    if (idP == NULL || idP->type != KjString)
    {
      resultsV[ix] = DB_ERR;
      continue;
    }

    int r = mongocEntityMergeOne(collP, idP->value.s, fragP, ts,
                                  &reportsV[ix], &snapshotsV[ix]);
    resultsV[ix] = r;
    if (r == DB_OK)
      anyOk = true;
  }

  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return anyOk ? DB_OK : DB_ERR;
}
