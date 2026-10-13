//
// FILE            fdscTokenCache.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <pthread.h>                                 // pthread_rwlock_*
#include <stdint.h>                                  // uint64_t
#include <stdlib.h>                                  // malloc, free
#include <string.h>                                  // memcmp, memcpy

#include "fdscIdentity.h"                            // FdscIdentity, fdscIdentityRef, fdscIdentityUnref
#include "fdscTokenCache.h"                          // Own interface



// -----------------------------------------------------------------------------
//
// The cache - a fixed table, a token in the slot its hash names; a new token takes the slot over
//
#define SLOTS      1024
#define TOKEN_MAX  (16 * 1024)                       // a bigger token is checked every time, never kept

typedef struct Slot
{
  uint64_t       hash;
  char*          token;
  int            len;
  FdscIdentity*  idP;                                // the cache's reference
} Slot;

//
// A lock per group of slots, not one for the table: every request of every thread looks a token up, and one
// lock would be one cache line all the cores write to (a read lock is a write to the lock)
//
#define LOCKS  64

static Slot              slotV[SLOTS];
static pthread_rwlock_t  lockV[LOCKS];
static pthread_once_t    lockOnce   = PTHREAD_ONCE_INIT;
static unsigned int      generation = 0;             // one more at each Clear - read and written atomically, changed with every lock held

static void locksInit(void)
{
  for (int i = 0; i < LOCKS; i++)
    pthread_rwlock_init(&lockV[i], NULL);
}

static pthread_rwlock_t* lockOf(Slot* sP)
{
  pthread_once(&lockOnce, locksInit);
  return &lockV[(sP - slotV) % LOCKS];
}



// -----------------------------------------------------------------------------
//
// hashOf - the slot's hash: the token's LAST 32 bytes (its signature's - random, made by the verifier's key)
// and its length, 8 bytes at a time. Not the whole token: a token is 1-4 KB (the credential is in it), and
// a byte-at-a-time hash of all of it was the biggest single cost of a request with auth. Which token a slot
// holds is decided by memcmp of the whole token, never by the hash; and only a token whose signature held
// is ever put in a slot.
//
static uint64_t hashOf(const char* s, int len)
{
  uint64_t h = 0xcbf29ce484222325ULL ^ (uint64_t) len;

  if (len < 32)
  {
    for (int i = 0; i < len; i++)
    {
      h ^= (unsigned char) s[i];
      h *= 0x100000001b3ULL;
    }

    return h;
  }

  for (int i = len - 32; i < len; i += 8)
  {
    uint64_t w;

    memcpy(&w, &s[i], 8);
    h ^= w;
    h *= 0x9e3779b97f4a7c15ULL;
    h ^= h >> 29;
  }

  return h;
}



// -----------------------------------------------------------------------------
//
// fdscTokenCacheGet -
//
FdscIdentity* fdscTokenCacheGet(const char* token, int len)
{
  uint64_t      h   = hashOf(token, len);
  Slot*         sP  = &slotV[h % SLOTS];
  FdscIdentity* idP = NULL;
  pthread_rwlock_t* lockP = lockOf(sP);

  pthread_rwlock_rdlock(lockP);

  if ((sP->token != NULL) && (sP->hash == h) && (sP->len == len) && (memcmp(sP->token, token, len) == 0))
  {
    idP = sP->idP;
    fdscIdentityRef(idP);
  }

  pthread_rwlock_unlock(lockP);
  return idP;
}



// -----------------------------------------------------------------------------
//
// fdscTokenCacheGeneration -
//
unsigned int fdscTokenCacheGeneration(void)
{
  return __atomic_load_n(&generation, __ATOMIC_ACQUIRE);
}



// -----------------------------------------------------------------------------
//
// fdscTokenCacheAdd -
//
void fdscTokenCacheAdd(const char* token, int len, FdscIdentity* idP, unsigned int checkedWith)
{
  if ((len > TOKEN_MAX) || (idP == NULL))
    return;

  char* copy = (char*) malloc(len);

  if (copy == NULL)
    return;

  memcpy(copy, token, len);

  uint64_t h  = hashOf(token, len);
  Slot*    sP = &slotV[h % SLOTS];
  pthread_rwlock_t* lockP = lockOf(sP);

  pthread_rwlock_wrlock(lockP);

  if (checkedWith != __atomic_load_n(&generation, __ATOMIC_ACQUIRE))   // the keys changed while it was checked (Clear holds this lock too while it changes)
  {
    pthread_rwlock_unlock(lockP);
    free(copy);
    return;
  }

  char*         oldP   = sP->token;
  FdscIdentity* oldIdP = sP->idP;

  fdscIdentityRef(idP);
  sP->hash  = h;
  sP->token = copy;
  sP->len   = len;
  sP->idP   = idP;
  pthread_rwlock_unlock(lockP);

  free(oldP);
  fdscIdentityUnref(oldIdP);
}



// -----------------------------------------------------------------------------
//
// fdscTokenCacheClear -
//
void fdscTokenCacheClear(void)
{
  pthread_once(&lockOnce, locksInit);

  for (int i = 0; i < LOCKS; i++)                    // always in this order - nothing else holds two
    pthread_rwlock_wrlock(&lockV[i]);

  for (int i = 0; i < SLOTS; i++)
  {
    free(slotV[i].token);
    fdscIdentityUnref(slotV[i].idP);
    slotV[i].token = NULL;
    slotV[i].len   = 0;
    slotV[i].idP   = NULL;
  }

  __atomic_add_fetch(&generation, 1, __ATOMIC_RELEASE);

  for (int i = 0; i < LOCKS; i++)
    pthread_rwlock_unlock(&lockV[i]);
}
