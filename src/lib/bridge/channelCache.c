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

#include "khash/khash.h"                              // KHashTable, khashTableCreate, khashItemAdd, khashItemLookup, khashItemRemove
#include "ktrace/kTrace.h"                            // KT_I, KT_T

#include "corBridge/BridgeDriver.h"                   // BridgeDriver, bridges, bridgeCount

#include "bridge/Channel.h"                           // Channel
#include "bridge/channelCache.h"                      // Own interface
#include "coraineTraceLevels.h"                       // KtBridge



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
// ⭐ The second index is therefore NOT a second hash. khashItemAdd does no
// lookup at all - it prepends - and khashItemRemove unlinks the first match and
// reports success. A structure kept in two hashes that fall out of step gives
// you a Channel that still delivers and cannot be deleted. One hash, one list,
// and the list is authoritative.
//
static KHashTable*  endpointHash  = NULL;
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
static int          requestCounter = 0;          // Channels that ASK something - a service or an action



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
// khash does not keep the key: it hands the lookup name and the stored data to
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

  endpointHash = khashTableCreate(NULL, channelHash, channelCompare, 128);
  if (endpointHash == NULL)
    return CHANNEL_ERR;

  channelList    = NULL;
  channelCounter = 0;
  requestCounter = 0;

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

  return (Channel*) khashItemLookup(endpointHash, key);
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

  if ((entityId == NULL) || (entityType == NULL) || (attrName == NULL))
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
  clashP = channelLookupByTarget(tenantP, entityId, attrName);
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
  channelP->entityId   = strdup(entityId);
  channelP->entityType = strdup(entityType);
  channelP->attrName   = strdup(attrName);

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

  if (khashItemAdd(endpointHash, key, channelP) != 0)
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
    free(channelP);
    return CHANNEL_ERR;
  }

  channelP->next = channelList;
  __atomic_store_n(&channelList, channelP, __ATOMIC_RELEASE);   // published complete - see cacheLock
  channelCounter++;

  if (channelP->kind != BridgeChannelTopic)
    requestCounter++;

  KT_T(KtBridge, "channel '%s' on bridge '%s' -> %s/%s (%s)",
       endpoint, bridgeName, entityId, attrName,
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
  int r = createLocked(id, bridgeName, endpoint, kind, direction, retention, tenantP, entityId, entityType, attrName, clashPP);
  pthread_rwlock_unlock(&cacheLock);

  return r;
}



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

  Channel* channelP = (Channel*) khashItemLookup(endpointHash, key);
  if (channelP == NULL)
    return CHANNEL_ERR;

  khashItemRemove(endpointHash, key);

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

  if (channelP->kind != BridgeChannelTopic)
    requestCounter--;

  free(channelP->id);
  free(channelP->bridgeName);
  free(channelP->endpoint);
  free(channelP->entityId);
  free(channelP->entityType);
  free(channelP->attrName);
  free(channelP->statusReason);
  free(channelP->notifyUri);
  free(channelP->notifyAccept);
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
// channelRequestCount -
//
int channelRequestCount(void)
{
  return requestCounter;
}
