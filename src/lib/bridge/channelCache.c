//
// FILE            channelCache.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdlib.h>                                   // malloc, free
#include <string.h>                                   // strcmp, strdup, snprintf
#include <stdio.h>                                    // snprintf
#include <pthread.h>                                  // pthread_rwlock_*

#include "corHash/corHash.h"                          // CorHashTable, corHashTableCreate, corHashItemAdd, corHashItemLookup, corHashItemRemove
#include "corLog/corLog.h"                            // COR_I, COR_T

#include "corBridge/BridgeDriver.h"                   // BridgeDriver, bridges, bridgeCount
#include "corBridge/BridgeBroker.h"                   // BRIDGE_*, BridgeChannelStatus

#include "bridge/Channel.h"                           // Channel
#if COR_FEATURE_BRIDGE_RECORDS
#include "bridge/recordMap.h"                         // recordMapFree
#endif
#include "bridge/channelCache.h"                      // Own interface
#include "coraineTraceLevels.h"                       // CtBridge



// -----------------------------------------------------------------------------
//
// The cache
//
// A hash on the hot key and a list for everything else, deliberately:
//
// channelLookup runs once per arriving sample, on a transport thread, so it
// gets the hash. Everything else - creating, deleting, the reverse lookup that
// enforces one writer per attribute - happens at configuration time, where a
// walk over a handful of Channels costs nothing worth measuring.
//
// ⭐ The second index is therefore NOT a second hash. corHashItemAdd does no
// lookup at all - it prepends - and corHashItemRemove unlinks the first match and
// reports success. A structure kept in two hashes that fall out of step gives
// you a Channel that still delivers and cannot be deleted. One hash, one list,
// and the list is authoritative.
//
static CorHashTable*  endpointHash  = NULL;
static Channel*     channelList   = NULL;

//
// ⭐ CHANNELS ARE MADE AT RUN TIME TOO - a service or an action a transport
// discovers (bridgeEndpointDiscoveredIn) - while plugin threads look them up
// on every sample and request threads walk them. So:
//   - the HASH is read under cacheLock (read), changed under it (write);
//   - the LIST is walked with no lock: a Channel is complete before it is
//     linked in, it is linked in at the HEAD with a release store, and a
//     reader loads the head with acquire - it sees the list as it was, or with
//     the new Channel, never half of one. Nothing already linked changes.
//   - a Channel is never freed while the bridges run (channelDelete below).
//
static pthread_rwlock_t cacheLock = PTHREAD_RWLOCK_INITIALIZER;
static int          channelCounter = 0;
static int          outCounter     = 0;          // Channels that SEND - anything not inbound only



// -----------------------------------------------------------------------------
//
// channelKey - the hot-path key: bridge and endpoint, not endpoint alone
//
// Two Bridges of the same kind - two DDS participants on two domains - may both
// carry a topic called 'rt/pose', and they are different Channels.
//
// The separator is a TAB, and channelCreate REFUSES an endpoint containing one
// rather than assuming none does. Two different pairs colliding into one key
// would be a Channel delivering another Channel's samples, and "no real wire
// names a topic with a tab in it" is an assumption about every transport that
// will ever be written, made in a comment, load-bearing, and unenforced. It
// costs one strchr at configuration time to not have to be right about it.
//
#define CHANNEL_KEY_SEPARATOR '\t'

static void channelKey(const char* bridgeName, const char* endpoint, char* keyOut, int keyOutSize)
{
  snprintf(keyOut, keyOutSize, "%s%c%s", bridgeName, CHANNEL_KEY_SEPARATOR, endpoint);
}



// -----------------------------------------------------------------------------
//
// channelHash - djb2 over the composite key
//
static unsigned int channelHash(const char* name)
{
  unsigned int hash = 5381;

  while (*name != 0)
  {
    hash = ((hash << 5) + hash) + (unsigned char) *name;
    name++;
  }

  return hash;
}



// -----------------------------------------------------------------------------
//
// channelCompare - does this stored Channel answer to this key?
//
// corHash does not keep the key: it hands the lookup name and the stored data to
// this function. So the Channel has to be able to rebuild its own key, which it
// can - it holds both halves.
//
static int channelCompare(const char* name, void* itemP)
{
  Channel* channelP = (Channel*) itemP;
  char     key[512];

  channelKey(channelP->bridgeName, channelP->endpoint, key, sizeof(key));

  return strcmp(name, key);
}



// -----------------------------------------------------------------------------
//
// channelCacheInit -
//
int channelCacheInit(void)
{
  if (endpointHash != NULL)
    return CHANNEL_OK;

  endpointHash = corHashTableCreate(NULL, channelHash, channelCompare, 128);
  if (endpointHash == NULL)
    return CHANNEL_ERR;

  channelList    = NULL;
  channelCounter = 0;
  outCounter = 0;

  return CHANNEL_OK;
}



// -----------------------------------------------------------------------------
//
// channelLookup - THE HOT PATH
//
static Channel* lookupLocked(const char* bridgeName, const char* endpoint)
{
  char key[512];
  channelKey(bridgeName, endpoint, key, sizeof(key));

  return (Channel*) corHashItemLookup(endpointHash, key);
}

Channel* channelLookup(const char* bridgeName, const char* endpoint)
{
  if ((endpointHash == NULL) || (bridgeName == NULL) || (endpoint == NULL))
    return NULL;

  pthread_rwlock_rdlock(&cacheLock);
  Channel* channelP = lookupLocked(bridgeName, endpoint);
  pthread_rwlock_unlock(&cacheLock);

  return channelP;
}



// -----------------------------------------------------------------------------
//
// channelLookupByTarget - the reverse index, and why it is a walk
//
Channel* channelLookupByTarget(Tenant* tenantP, const char* entityId, const char* attrName)
{
  if ((entityId == NULL) || (attrName == NULL))
    return NULL;

  for (Channel* channelP = __atomic_load_n(&channelList, __ATOMIC_ACQUIRE); channelP != NULL; channelP = channelP->next)
  {
    if (channelP->tenantP != tenantP)
      continue;

    if (channelP->recordMapP != NULL)                 // a record Channel writes no one attribute
      continue;

    if ((strcmp(channelP->entityId, entityId) == 0) && (strcmp(channelP->attrName, attrName) == 0))
      return channelP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// bridgeLookup - which loaded plugin answers to this name
//
static BridgeDriver* bridgeLookup(const char* bridgeName)
{
  for (int i = 0; i < bridgeCount; i++)
  {
    if ((bridges[i].alias != NULL) && (strcmp(bridges[i].alias, bridgeName) == 0))
      return &bridges[i];
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// channelCreate -
//
static int createLocked
(
  const char*        id,
  const char*        bridgeName,
  const char*        endpoint,
  BridgeChannelKind  kind,
  BridgeDirection    direction,
  ChannelRetention   retention,
  Tenant*            tenantP,
  const char*        entityId,
  const char*        entityType,
  const char*        attrName,
  struct RecordMap*  recordMapP,
  Channel**          clashPP
)
{
  if (clashPP != NULL)
    *clashPP = NULL;

  if ((bridgeName == NULL) || (endpoint == NULL) || (*endpoint == 0))
    return CHANNEL_BAD_INPUT;

  //
  // See channelKey: the composite key would be ambiguous, and the collision
  // would show up as one Channel receiving another's samples.
  //
  if ((strchr(endpoint, CHANNEL_KEY_SEPARATOR) != NULL) || (strchr(bridgeName, CHANNEL_KEY_SEPARATOR) != NULL))
    return CHANNEL_BAD_INPUT;

  if ((recordMapP == NULL) && ((entityId == NULL) || (entityType == NULL) || (attrName == NULL)))
    return CHANNEL_BAD_INPUT;

  if (endpointHash == NULL)
    return CHANNEL_ERR;

  //
  // Check 1 - is this endpoint already carried?
  //
  // The second Channel could never be reached: a lookup answers with one of
  // them and nothing says which.
  //
  Channel* clashP = lookupLocked(bridgeName, endpoint);
  if (clashP != NULL)
  {
    if (clashPP != NULL)
      *clashPP = clashP;
    return CHANNEL_DUP_ENDPOINT;
  }

  //
  // Check 2 - is this attribute already written by another Channel?
  //
  // Both WOULD be reached, and they race: the value of the attribute then
  // depends on which transport delivered last. One writer per attribute.
  //
  clashP = (recordMapP == NULL) ? channelLookupByTarget(tenantP, entityId, attrName) : NULL;
  if (clashP != NULL)
  {
    if (clashPP != NULL)
      *clashPP = clashP;
    return CHANNEL_DUP_TARGET;
  }

  Channel* channelP = (Channel*) calloc(1, sizeof(Channel));
  if (channelP == NULL)
    return CHANNEL_ERR;

  channelP->id         = (id != NULL) ? strdup(id) : NULL;
  channelP->bridgeName = strdup(bridgeName);
  channelP->endpoint   = strdup(endpoint);
  channelP->kind       = kind;
  channelP->direction  = direction;
  channelP->retention  = retention;
  channelP->tenantP    = tenantP;
  channelP->entityId   = (entityId   != NULL) ? strdup(entityId)   : NULL;
  channelP->entityType = (entityType != NULL) ? strdup(entityType) : NULL;
  channelP->attrName   = (attrName   != NULL) ? strdup(attrName)   : NULL;
  channelP->recordMapP = recordMapP;

  //
  // Check 3 - is the Bridge it names actually here?
  //
  // If not, the Channel is still created. It is valid configuration naming a
  // transport this build was not started with, and a stored object must not
  // prevent a boot. Dormant is what makes that visible, which 'no data
  // arriving' is not.
  //
  BridgeDriver* driverP = bridgeLookup(bridgeName);
  if (driverP == NULL)
  {
    static char reason[256];
    snprintf(reason, sizeof(reason), "no bridge plugin named '%s' is loaded", bridgeName);
    channelP->status       = ChannelStatusDormant;
    channelP->statusReason = strdup(reason);
  }
  else
    channelP->status = ChannelStatusAvailable;

  char key[512];
  channelKey(bridgeName, endpoint, key, sizeof(key));

  if (corHashItemAdd(endpointHash, key, channelP) != 0)
  {
    free(channelP->id);
    free(channelP->bridgeName);
    free(channelP->endpoint);
    free(channelP->entityId);
    free(channelP->entityType);
    free(channelP->attrName);
    free(channelP->statusReason);
    free(channelP->notifyUri);
    free(channelP->notifyAccept);
    free(channelP->info);
    free(channelP);
    return CHANNEL_ERR;
  }

  channelP->next = channelList;
  __atomic_store_n(&channelList, channelP, __ATOMIC_RELEASE);   // published complete - see cacheLock
  channelCounter++;

  if (channelP->direction != BridgeDirectionIn)
    outCounter++;

  COR_T(CtBridge, "channel '%s' on bridge '%s' -> %s/%s (%s)",
        endpoint, bridgeName, (entityId != NULL) ? entityId : "records", (attrName != NULL) ? attrName : "-",
        (channelP->status == ChannelStatusAvailable) ? "available" : "dormant");

  return CHANNEL_OK;
}



// -----------------------------------------------------------------------------
//
// channelCreate - see channelCache.h; createLocked under cacheLock (write)
//
int channelCreate
(
  const char*        id,
  const char*        bridgeName,
  const char*        endpoint,
  BridgeChannelKind  kind,
  BridgeDirection    direction,
  ChannelRetention   retention,
  Tenant*            tenantP,
  const char*        entityId,
  const char*        entityType,
  const char*        attrName,
  Channel**          clashPP
)
{
  pthread_rwlock_wrlock(&cacheLock);
  int r = createLocked(id, bridgeName, endpoint, kind, direction, retention, tenantP, entityId, entityType, attrName, NULL, clashPP);
  pthread_rwlock_unlock(&cacheLock);

  return r;
}



#if COR_FEATURE_BRIDGE_RECORDS
// -----------------------------------------------------------------------------
//
// channelRecordsCreate - see channelCache.h; createLocked under cacheLock (write)
//
int channelRecordsCreate(const char* bridgeName, const char* endpoint, Tenant* tenantP, struct RecordMap* recordMapP, Channel** clashPP)
{
  if (recordMapP == NULL)
    return CHANNEL_BAD_INPUT;

  pthread_rwlock_wrlock(&cacheLock);
  int r = createLocked(NULL, bridgeName, endpoint, BridgeChannelTopic, BridgeDirectionIn, ChannelRetentionMirror, tenantP, NULL, NULL, NULL, recordMapP, clashPP);
  pthread_rwlock_unlock(&cacheLock);

  return r;
}
#endif



#if COR_FEATURE_CHANNEL_STATUS_IN
// -----------------------------------------------------------------------------
//
// bridgeChannelStatusIn - BridgeBroker.channelStatusIn, ABI 12
//
int bridgeChannelStatusIn(const char* bridgeName, const char* endpoint, int status, const char* reason)
{
  if ((bridgeName == NULL) || (endpoint == NULL))
    return BRIDGE_BAD_INPUT;

  if ((status != BridgeChannelAvailable) && (status != BridgeChannelDormant))
    return BRIDGE_BAD_INPUT;

  ChannelStatus channelStatus = (status == BridgeChannelAvailable) ? ChannelStatusAvailable : ChannelStatusDormant;

  if (channelStatusSet(bridgeName, endpoint, channelStatus, reason) != CHANNEL_OK)
    return BRIDGE_NOT_FOUND;

  COR_I("bridge '%s': channel '%s' is %s%s%s", bridgeName, endpoint,
        (channelStatus == ChannelStatusAvailable) ? "available" : "dormant",
        (reason != NULL) ? ": " : "", (reason != NULL) ? reason : "");

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// channelStatusSet - see channelCache.h
//
// A GET /channels on a worker thread may be rendering the reason being replaced, so the old one is
// not freed here: it is kept with the Channel and freed with it. A status changes a handful of times
// in a Channel's life, not per sample.
//
int channelStatusSet(const char* bridgeName, const char* endpoint, ChannelStatus status, const char* reason)
{
  Channel* channelP = channelLookup(bridgeName, endpoint);

  if (channelP == NULL)
    return CHANNEL_ERR;

  char* newReason = (reason != NULL) ? strdup(reason) : NULL;

  pthread_rwlock_wrlock(&cacheLock);

  char*  oldReason = channelP->statusReason;
  char** retiredV  = (oldReason != NULL) ? (char**) realloc(channelP->retiredReasonV, (channelP->retiredReasons + 1) * sizeof(char*)) : NULL;

  if (retiredV != NULL)
  {
    retiredV[channelP->retiredReasons++] = oldReason;
    channelP->retiredReasonV             = retiredV;
  }

  if ((oldReason == NULL) || (retiredV != NULL))
    __atomic_store_n(&channelP->statusReason, newReason, __ATOMIC_RELEASE);
  else
    free(newReason);                                  // out of memory: the old reason stays rather than be freed under a reader

  __atomic_store_n(&channelP->status, status, __ATOMIC_RELEASE);

  pthread_rwlock_unlock(&cacheLock);

  return CHANNEL_OK;
}
#endif



// -----------------------------------------------------------------------------
//
// deleteLocked - channelDelete's body, under cacheLock (write)
//
static int deleteLocked(const char* bridgeName, const char* endpoint)
{
  if ((endpointHash == NULL) || (bridgeName == NULL) || (endpoint == NULL))
    return CHANNEL_BAD_INPUT;

  char key[512];
  channelKey(bridgeName, endpoint, key, sizeof(key));

  Channel* channelP = (Channel*) corHashItemLookup(endpointHash, key);
  if (channelP == NULL)
    return CHANNEL_ERR;

  corHashItemRemove(endpointHash, key);

  //
  // Unlink from the list, which is the authoritative side. If this ever fails
  // to find what the hash just gave us, the two have fallen out of step and the
  // cache is not to be trusted - so it is not silently tolerated.
  //
  Channel** prevPP = &channelList;
  while (*prevPP != NULL)
  {
    if (*prevPP == channelP)
    {
      *prevPP = channelP->next;
      break;
    }
    prevPP = &(*prevPP)->next;
  }

  if (channelP->direction != BridgeDirectionIn)
    outCounter--;

  free(channelP->id);
  free(channelP->bridgeName);
  free(channelP->endpoint);
  free(channelP->entityId);
  free(channelP->entityType);
  free(channelP->attrName);
  free(channelP->statusReason);
  free(channelP->notifyUri);
  free(channelP->notifyAccept);
  free(channelP->info);

  for (int ix = 0; ix < channelP->retiredReasons; ix++)
    free(channelP->retiredReasonV[ix]);
  free(channelP->retiredReasonV);

#if COR_FEATURE_BRIDGE_RECORDS
  recordMapFree(channelP->recordMapP);
#endif

  free(channelP);

  channelCounter--;

  return CHANNEL_OK;
}



// -----------------------------------------------------------------------------
//
// channelDelete -
//
// ⚠ FREES the Channel, and the list is walked without a lock (see cacheLock):
// never while the bridges run. Nothing calls it at run time today.
//
int channelDelete(const char* bridgeName, const char* endpoint)
{
  pthread_rwlock_wrlock(&cacheLock);
  int r = deleteLocked(bridgeName, endpoint);
  pthread_rwlock_unlock(&cacheLock);

  return r;
}



// -----------------------------------------------------------------------------
//
// channelCacheFirst / channelCount -
//
Channel* channelCacheFirst(void)
{
  return __atomic_load_n(&channelList, __ATOMIC_ACQUIRE);
}

int channelCount(void)
{
  return channelCounter;
}



// -----------------------------------------------------------------------------
//
// channelOutCount -
//
int channelOutCount(void)
{
  return outCounter;
}
