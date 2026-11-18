//
// FILE            seRetention.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // calloc, free
#include <string.h>                                   // strcmp, strdup
#include <pthread.h>                                  // pthread_mutex_*

#include "db/Tenant.h"                                // Tenant
#include "serviceExecution/seRetention.h"             // Own interface



int seRetentionMaxEnded = 1000;



// -----------------------------------------------------------------------------
//
// Ring - one tenant's ended executions, by name: a Tenant can be released, its name stays its name
//
typedef struct Ring
{
  char          tenant[64];
  char**        ids;                                  // seRetentionMaxEnded of them
  int           first;                                // the oldest
  int           count;
  bool          seeded;
  struct Ring*  next;
} Ring;

static Ring*            rings = NULL;
static pthread_mutex_t  ringMutex = PTHREAD_MUTEX_INITIALIZER;



static Ring* ringOf(Tenant* tenantP)                  // under ringMutex
{
  const char* name = (tenantP != NULL) ? tenantP->name : "";

  for (Ring* rP = rings; rP != NULL; rP = rP->next)
  {
    if (strcmp(rP->tenant, name) == 0)
      return rP;
  }

  Ring* rP = (Ring*) calloc(1, sizeof(Ring));

  if (rP == NULL)
    return NULL;

  rP->ids = (char**) calloc(seRetentionMaxEnded, sizeof(char*));

  if (rP->ids == NULL)
  {
    free(rP);
    return NULL;
  }

  snprintf(rP->tenant, sizeof(rP->tenant), "%s", name);
  rP->next = rings;
  rings    = rP;

  return rP;
}



// -----------------------------------------------------------------------------
//
// seRetentionPush -
//
char* seRetentionPush(Tenant* tenantP, const char* execId)
{
  if ((seRetentionMaxEnded <= 0) || (execId == NULL))
    return NULL;

  char* idCopy = strdup(execId);

  if (idCopy == NULL)
    return NULL;

  char* evicted = NULL;

  pthread_mutex_lock(&ringMutex);

  Ring* rP = ringOf(tenantP);

  if (rP == NULL)
  {
    pthread_mutex_unlock(&ringMutex);
    free(idCopy);
    return NULL;
  }

  if (rP->count == seRetentionMaxEnded)               // full: the oldest goes, its slot takes the new one
  {
    evicted                = rP->ids[rP->first];
    rP->ids[rP->first]     = idCopy;
    rP->first              = (rP->first + 1) % seRetentionMaxEnded;
  }
  else
  {
    rP->ids[(rP->first + rP->count) % seRetentionMaxEnded] = idCopy;
    rP->count += 1;
  }

  pthread_mutex_unlock(&ringMutex);

  return evicted;
}



// -----------------------------------------------------------------------------
//
// seRetentionSeeded -
//
bool seRetentionSeeded(Tenant* tenantP)
{
  bool seeded = true;

  pthread_mutex_lock(&ringMutex);

  Ring* rP = ringOf(tenantP);

  if ((rP != NULL) && (rP->seeded == false))
  {
    rP->seeded = true;
    seeded     = false;
  }

  pthread_mutex_unlock(&ringMutex);

  return seeded;
}
