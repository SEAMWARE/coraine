//
// FILE            mongocHaWatch.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stddef.h>                                    // NULL
#include <string.h>                                    // strcmp, strncmp, strlen
#include <unistd.h>                                    // sleep
#include <pthread.h>                                   // pthread_create, pthread_detach

#include <mongoc/mongoc.h>                             // mongoc_*

#include "kalloc/kalloc.h"                             // kaBufferInit, kaBufferReset
#include "kjson/kjLookup.h"                            // kjLookup
#include "ktrace/kTrace.h"                             // KT_*

#include "db/DbDriver.h"                               // DB_OK, DB_ERR
#include "db/Tenant.h"                                 // Tenant, tenant0, tenantLookup, tenantGetOrCreate
#include "ha/HaEvent.h"                                // HaEvent, HaApplyFunc
#include "ha/haInit.h"                                 // haApplyWait

#include "currentState/mongoc/mongocGlobals.h"         // mongocDbName, mongocGlobalDb, mongocUriString
#include "currentState/mongoc/mongocBsonToKjTree.h"    // mongocBsonToKjTree
#include "currentState/mongoc/mongocHaWatch.h"         // Own interface



// -----------------------------------------------------------------------------
//
// Shared from mongocInit.c
//
extern mongoc_client_pool_t* poolP;



// -----------------------------------------------------------------------------
//
// The change stream's own parse arena
//
// NOT corRest.kalloc: the apply resets that one for what IT reads from the
// database, and the event tree - which the HaEvent's id points into - has to
// outlive the apply. One arena per owner, each reset by its owner.
//
static KAlloc  haAlloc;
static char    haAllocBuffer[8 * 1024];



// -----------------------------------------------------------------------------
//
// haApply - what to do with an event, handed over by the broker at start
//
// Not called by name: a DB plugin is a shared object of its own, and a broker
// symbol it references but the broker itself does not is dropped at link time -
// the plugin then fails to LOAD, at run time, on a build that compiled cleanly.
//
static HaApplyFunc  haApply = NULL;



// -----------------------------------------------------------------------------
//
// replicaSetCheck - is the mongod we are talking to part of a replica set?
//
// Change streams read the oplog, and a standalone mongod has none - it answers
// every watch with "The $changeStream stage is only supported on replica sets".
//
// The check ASKS THE SERVER rather than looking at the connection options. A
// single-node replica set is reached perfectly well over a direct connection
// with no replicaSet= in the URI, so going by the URI would refuse a deployment
// that is in fact perfectly able to run.
//
static bool replicaSetCheck(void)
{
  mongoc_client_t* clientP = mongoc_client_pool_pop(poolP);

  if (clientP == NULL)
  {
    KT_E("HA: no mongo connection to check the deployment with");
    return false;
  }

  bson_t*       commandP     = BCON_NEW("isMaster", BCON_INT32(1));
  bson_t        reply;
  bson_error_t  error;
  bool          isReplicaSet = false;

  if (mongoc_client_command_simple(clientP, "admin", commandP, NULL, &reply, &error) == true)
  {
    bson_iter_t iter;

    // A replica set member names its set; a standalone has no 'setName'
    if (bson_iter_init_find(&iter, &reply, "setName") == true)
      isReplicaSet = true;
  }
  else
    KT_E("HA: unable to ask mongo whether it is a replica set (%s)", error.message);

  bson_destroy(&reply);
  bson_destroy(commandP);
  mongoc_client_pool_push(poolP, clientP);

  return isReplicaSet;
}



// -----------------------------------------------------------------------------
//
// tenantOfDatabase - which tenant does this database belong to?
//
// The default tenant's database is mongocDbName itself; every other tenant's is
// "<mongocDbName>-<tenant>". Anything else in the deployment is not ours - the
// watch is cluster-wide, so other applications' databases come past too.
//
// A tenant this instance has never heard of is CREATED, caches and all: another
// instance inventing a tenant is exactly the kind of thing HA has to learn
// about. Its indexes are not set up here - the instance that invented it did
// that, on this very database, and an apply does not write.
//
static Tenant* tenantOfDatabase(const char* db)
{
  int dbNameLen = strlen(mongocDbName);

  if (strcmp(db, mongocDbName) == 0)
    return &tenant0;

  if ((strncmp(db, mongocDbName, dbNameLen) == 0) && (db[dbNameLen] == '-') && (db[dbNameLen + 1] != 0))
    return tenantGetOrCreate(&db[dbNameLen + 1]);

  return NULL;
}



// -----------------------------------------------------------------------------
//
// eventTreat - turn one change-stream document into an HaEvent and apply it
//
static void eventTreat(const bson_t* bsonP)
{
  //
  // Before anything else, including working out whose database this is: doing
  // that CREATES a tenant, and the caches are not loaded yet.
  //
  haApplyWait();

  KjNode* eventP = mongocBsonToKjTree(&haAlloc, bsonP);

  if (eventP == NULL)
  {
    KT_E("HA: unable to parse a change stream event");
    return;
  }

  KjNode* opTypeP = kjLookup(eventP, "operationType");
  KjNode* nsP     = kjLookup(eventP, "ns");

  if ((opTypeP == NULL) || (opTypeP->type != KjString) || (nsP == NULL))
  {
    KT_E("HA: change stream event without operationType or ns - ignored");
    return;
  }

  //
  // 'invalidate', 'drop', 'dropDatabase', 'rename' - the collection or the
  // database itself went away. There is nothing sensible to do per item; the
  // caches are rebuilt at the next start.
  //
  HaOp op;

  if      (strcmp(opTypeP->value.s, "insert")  == 0)  op = HaOpUpsert;
  else if (strcmp(opTypeP->value.s, "update")  == 0)  op = HaOpUpsert;
  else if (strcmp(opTypeP->value.s, "replace") == 0)  op = HaOpUpsert;
  else if (strcmp(opTypeP->value.s, "delete")  == 0)  op = HaOpDelete;
  else
  {
    KT_W("HA: change stream event '%s' - not an item change, ignored", opTypeP->value.s);
    return;
  }

  KjNode* dbP   = kjLookup(nsP, "db");
  KjNode* collP = kjLookup(nsP, "coll");

  if ((dbP == NULL) || (collP == NULL) || (dbP->type != KjString) || (collP->type != KjString))
  {
    KT_E("HA: change stream event with an incomplete 'ns' - ignored");
    return;
  }

  //
  // What is it, and whose is it?
  //
  // Subscriptions and registrations live in the tenant's own database.
  // @contexts do NOT: the context cache is global - an @context is identified by
  // its URL, and the document at a URL is the same whoever fetched it - and the
  // rows sit in the reserved global database. So a context event is accepted
  // from there and from nowhere else, and it carries no tenant at all.
  //
  HaEvent haEvent;

  if (strcmp(collP->value.s, "contexts") == 0)
  {
    if (strcmp(dbP->value.s, mongocGlobalDb) != 0)
      return;  // A collection called "contexts" in somebody else's database

    haEvent.kind    = HaContext;
    haEvent.tenantP = NULL;
  }
  else
  {
    if      (strcmp(collP->value.s, "subscriptions") == 0)  haEvent.kind = HaSubscription;
    else if (strcmp(collP->value.s, "registrations") == 0)  haEvent.kind = HaRegistration;
    else
      return;  // Entities, snapshots, and everybody else's collections

    haEvent.tenantP = tenantOfDatabase(dbP->value.s);

    if (haEvent.tenantP == NULL)
      return;  // Another application's database
  }

  //
  // The id. Every document this broker writes to these three collections has a
  // string _id (a URI, or the @context identifier).
  //
  // ⚠️ It is looked up as "id": mongocBsonToKjTree renames _id on the way in, and
  // that applies to the change event's documentKey as much as to a document. The
  // raw name is tried too, so this does not silently break if that ever changes.
  //
  KjNode* documentKeyP = kjLookup(eventP, "documentKey");
  KjNode* idP          = (documentKeyP != NULL)? kjLookup(documentKeyP, "id") : NULL;

  if ((idP == NULL) && (documentKeyP != NULL))
    idP = kjLookup(documentKeyP, "_id");

  if ((idP == NULL) || (idP->type != KjString))
  {
    KT_W("HA: change in %s.%s with no string _id - ignored", dbP->value.s, collP->value.s);
    return;
  }

  haEvent.op   = op;
  haEvent.id   = idP->value.s;
  haEvent.apiP = NULL;  // The mongo channel shares the database: the apply reads the document

  haApply(&haEvent);
}



// -----------------------------------------------------------------------------
//
// haWatchThread - one thread, one watch, every tenant and all three collections
//
// mongoc_client_watch() watches the whole deployment, so a single cursor covers
// every tenant's database - a tenant IS a database, and there is no telling in
// advance which ones exist. A watch per database would mean a thread per tenant
// and no way to notice a tenant that appears later.
//
// mongoc_change_stream_next() BLOCKS until something happens, so this thread
// costs nothing while the deployment is quiet. It is not a poll.
//
static void* haWatchThread(void* vP)
{
  mongoc_client_t* clientP = mongoc_client_new(mongocUriString);

  if (clientP == NULL)
  {
    KT_E("HA: unable to create a mongo client for the change stream - the caches will NOT be kept in sync");
    return NULL;
  }

  bson_t  pipeline = BSON_INITIALIZER;   // Empty: everything. The filtering is done in eventTreat

  //
  // maxAwaitTimeMS is how long the server holds the cursor open waiting for
  // something to happen. It is the difference between a blocked thread and a
  // spinning one.
  //
  bson_t* optsP = BCON_NEW("maxAwaitTimeMS", BCON_INT32(1000));

  while (1)
  {
    mongoc_change_stream_t* streamP = mongoc_client_watch(clientP, &pipeline, optsP);
    const bson_t*           bsonP;
    bson_error_t            error;
    const bson_t*           reply;

    KT_I("HA: watching the database for changes made by the other broker instances");

    while (1)
    {
      if (mongoc_change_stream_next(streamP, &bsonP) == true)
      {
        //
        // ⚠️ KTRUE = REUSE. With KFALSE the blocks are freed but the list that
        // holds them is left dangling - the next event frees them again.
        //
        kaBufferReset(&haAlloc, KTRUE);
        eventTreat(bsonP);
        continue;
      }

      //
      // ⚠️ next() returning false does NOT mean the stream is finished - it
      // means "nothing right now", every maxAwaitTimeMS, for as long as the
      // deployment is quiet. Only error_document() tells the two apart, and
      // treating a quiet moment as a broken stream tears the watch down and
      // rebuilds it once a second, forever, without ever delivering an event.
      //
      if (mongoc_change_stream_error_document(streamP, &error, &reply) == true)
        break;
    }

    //
    // mongoc resumes by itself over a transient error, so getting here means it
    // could not. Whatever was missed is picked up by a restart.
    //
    KT_E("HA: change stream error (%s) - restarting the stream in 5 seconds", error.message);

    mongoc_change_stream_destroy(streamP);
    sleep(5);
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// mongocHaWatchStart -
//
int mongocHaWatchStart(HaApplyFunc applyF)
{
  haApply = applyF;

  if (poolP == NULL)
  {
    KT_E("HA: the mongo plugin is not connected");
    return DB_ERR;
  }

  if (replicaSetCheck() == false)
  {
    KT_E("--ha mongo needs a mongo REPLICA SET - this mongod is a standalone, and a standalone has no oplog for a change stream to read. "
         "Either point the broker at a replica set (a single node is enough: mongod --replSet <name>, then rs.initiate()) or run without --ha");
    return DB_ERR;
  }

  kaBufferInit(&haAlloc, haAllocBuffer, sizeof(haAllocBuffer), 4096, NULL, "ha-watch");

  pthread_t tid;

  if (pthread_create(&tid, NULL, haWatchThread, NULL) != 0)
  {
    KT_E("HA: unable to create the change stream thread");
    return DB_ERR;
  }

  pthread_detach(tid);

  return DB_OK;
}
