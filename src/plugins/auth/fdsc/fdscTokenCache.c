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

#include "fdscTokenCache.h"                          // Own interface



// -----------------------------------------------------------------------------
//
// The cache - a fixed table, a token in the slot its hash names; a new token takes the slot over
//
#define SLOTS      1024
#define TOKEN_MAX  (16 * 1024)                       // a bigger token is checked every time, never kept

typedef struct Slot
{
  uint64_t  hash;
  char*     token;
  int       len;
} Slot;

static Slot              slotV[SLOTS];
static pthread_rwlock_t  lock       = PTHREAD_RWLOCK_INITIALIZER;
static unsigned int      generation = 0;             // one more at each Clear



// -----------------------------------------------------------------------------
//
// hashOf - FNV-1a
//
static uint64_t hashOf(const char* s, int len)
{
  uint64_t h = 0xcbf29ce484222325ULL;

  for (int i = 0; i < len; i++)
  {
    h ^= (unsigned char) s[i];
    h *= 0x100000001b3ULL;
  }

  return h;
}



// -----------------------------------------------------------------------------
//
// fdscTokenCacheHas -
//
bool fdscTokenCacheHas(const char* token, int len)
{
  uint64_t h     = hashOf(token, len);
  Slot*    sP    = &slotV[h % SLOTS];
  bool     found = false;

  pthread_rwlock_rdlock(&lock);
  found = (sP->token != NULL) && (sP->hash == h) && (sP->len == len) && (memcmp(sP->token, token, len) == 0);
  pthread_rwlock_unlock(&lock);

  return found;
}



// -----------------------------------------------------------------------------
//
// fdscTokenCacheAdd -
//
unsigned int fdscTokenCacheGeneration(void)
{
  pthread_rwlock_rdlock(&lock);
  unsigned int g = generation;
  pthread_rwlock_unlock(&lock);

  return g;
}



// -----------------------------------------------------------------------------
//
// fdscTokenCacheAdd -
//
void fdscTokenCacheAdd(const char* token, int len, unsigned int checkedWith)
{
  if (len > TOKEN_MAX)
    return;

  char* copy = (char*) malloc(len);

  if (copy == NULL)
    return;

  memcpy(copy, token, len);

  uint64_t h  = hashOf(token, len);
  Slot*    sP = &slotV[h % SLOTS];

  pthread_rwlock_wrlock(&lock);

  if (checkedWith != generation)                     // the keys changed while it was checked
  {
    pthread_rwlock_unlock(&lock);
    free(copy);
    return;
  }

  char* oldP = sP->token;
  sP->hash  = h;
  sP->token = copy;
  sP->len   = len;
  pthread_rwlock_unlock(&lock);

  free(oldP);
}



// -----------------------------------------------------------------------------
//
// fdscTokenCacheClear -
//
void fdscTokenCacheClear(void)
{
  pthread_rwlock_wrlock(&lock);

  for (int i = 0; i < SLOTS; i++)
  {
    free(slotV[i].token);
    slotV[i].token = NULL;
    slotV[i].len   = 0;
  }

  generation += 1;

  pthread_rwlock_unlock(&lock);
}
