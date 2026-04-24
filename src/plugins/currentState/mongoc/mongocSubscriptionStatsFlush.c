//
// FILE            mongocSubscriptionStatsFlush.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// HA-safe delta flush:
//   $inc { notification.timesSent: delta, notification.timesFailed: delta }
//   $set { notification.lastNotification: ns, ... }
//
// $inc composes under concurrent flushes from other broker instances, so we
// don't lose counts when N brokers flush close together. Last-* timestamps
// use $set unconditionally — it is fine for "last" to end up set to any
// of the concurrent values.
//
#include <stdint.h>                                  // uint64_t

#include <mongoc/mongoc.h>                           // mongoc_collection_t, mongoc_collection_update_one

#include "ktrace/kTrace.h"                           // KT_E

#include "db/DbDriver.h"                             // DB_OK, DB_NOT_FOUND, DB_ERR
#include "currentState/mongoc/mongocSubscriptionStatsFlush.h"  // Own interface



// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// mongocSubscriptionStatsFlush -
//
int mongocSubscriptionStatsFlush(Tenant*      tenantP,
                                 const char*  subId,
                                 int          deltaSent,
                                 int          deltaFailed,
                                 uint64_t     lastNotification,
                                 uint64_t     lastSuccess,
                                 uint64_t     lastFailure)
{
  // Nothing to do on empty delta with no timestamps — caller should skip,
  // but be defensive.
  if (deltaSent == 0 && deltaFailed == 0 &&
      lastNotification == 0 && lastSuccess == 0 && lastFailure == 0)
    return DB_OK;

  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "subscriptions");

  bson_t filter;
  bson_init(&filter);
  BSON_APPEND_UTF8(&filter, "_id", subId);

  bson_t update;
  bson_t incDoc;
  bson_t setDoc;
  bson_init(&update);
  bson_init(&incDoc);
  bson_init(&setDoc);

  bool hasInc = false;
  bool hasSet = false;

  if (deltaSent != 0)
  {
    BSON_APPEND_INT32(&incDoc, "notification.timesSent", deltaSent);
    hasInc = true;
  }
  if (deltaFailed != 0)
  {
    BSON_APPEND_INT32(&incDoc, "notification.timesFailed", deltaFailed);
    hasInc = true;
  }

  if (lastNotification != 0)
  {
    BSON_APPEND_INT64(&setDoc, "notification.lastNotification", (int64_t) lastNotification);
    hasSet = true;
  }
  if (lastSuccess != 0)
  {
    BSON_APPEND_INT64(&setDoc, "notification.lastSuccess", (int64_t) lastSuccess);
    hasSet = true;
  }
  if (lastFailure != 0)
  {
    BSON_APPEND_INT64(&setDoc, "notification.lastFailure", (int64_t) lastFailure);
    hasSet = true;
  }

  if (hasInc) BSON_APPEND_DOCUMENT(&update, "$inc", &incDoc);
  if (hasSet) BSON_APPEND_DOCUMENT(&update, "$set", &setDoc);

  int result = DB_OK;
  bson_t        reply;
  bson_error_t  error;

  if (!mongoc_collection_update_one(collP, &filter, &update, NULL, &reply, &error))
  {
    KT_E("mongoc: subscriptionStatsFlush(%s) failed: %s", subId, error.message);
    result = DB_ERR;
  }
  else
  {
    bson_iter_t iter;
    int64_t     matched = 0;
    if (bson_iter_init_find(&iter, &reply, "matchedCount"))
    {
      if      (BSON_ITER_HOLDS_INT32(&iter)) matched = bson_iter_int32(&iter);
      else if (BSON_ITER_HOLDS_INT64(&iter)) matched = bson_iter_int64(&iter);
    }
    if (matched == 0)
      result = DB_NOT_FOUND;
  }

  bson_destroy(&reply);
  bson_destroy(&update);
  bson_destroy(&incDoc);
  bson_destroy(&setDoc);
  bson_destroy(&filter);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}
