//
// FILE            bridgeDefaultEntity.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // calloc, free
#include <string.h>                                   // strcmp, strdup

#include "ktrace/kTrace.h"                            // KT_T

#include "corBridge/BridgeDriver.h"                   // BRIDGES_MAX

#include "bridge/bridgeDefaultEntity.h"               // Own interface
#include "coraineTraceLevels.h"                       // KtBridge



// -----------------------------------------------------------------------------
//
// BridgeDefaultEntity - one per bridge, at most
//
typedef struct BridgeDefaultEntity
{
  char*    bridgeName;
  char*    entityId;
  char*    entityType;                                // EXPANDED, like a Channel's
  Tenant*  tenantP;
  bool     created;                                   // has this process made the entity yet
} BridgeDefaultEntity;

static BridgeDefaultEntity  defaults[BRIDGES_MAX];
static int                  defaultCount = 0;



// -----------------------------------------------------------------------------
//
// bridgeDefaultEntityCount -
//
int bridgeDefaultEntityCount(void)
{
  return defaultCount;
}



// -----------------------------------------------------------------------------
//
// bridgeDefaultEntitySet -
//
bool bridgeDefaultEntitySet(const char* bridgeName, const char* entityId, const char* entityType, Tenant* tenantP)
{
  if ((bridgeName == NULL) || (*bridgeName == 0))
    return false;

  if (defaultCount >= BRIDGES_MAX)
    return false;

  for (int i = 0; i < defaultCount; i++)
  {
    if (strcmp(defaults[i].bridgeName, bridgeName) == 0)
      return false;                                    // said twice in one file - the first one stands
  }

  //
  // The derived id: "urn:ngsi-ld:<alias>:default", which for the dds bridge is
  // the one already in use elsewhere. A rule, not a special case.
  //
  // The TYPE is not derived here, and deliberately: it has to reach the store
  // expanded, and expansion is the configuration loader's job - it is where
  // the core context is already in hand and where every other name in this
  // file is expanded exactly once.
  //
  char derivedId[256];

  if (entityId == NULL)
  {
    snprintf(derivedId, sizeof(derivedId), "urn:ngsi-ld:%s:default", bridgeName);
    entityId = derivedId;
  }

  if ((entityType == NULL) || (*entityType == 0))
    return false;

  defaults[defaultCount].bridgeName = strdup(bridgeName);
  defaults[defaultCount].entityId   = strdup(entityId);
  defaults[defaultCount].entityType = strdup(entityType);
  defaults[defaultCount].tenantP    = tenantP;
  defaults[defaultCount].created    = false;

  if ((defaults[defaultCount].bridgeName == NULL) ||
      (defaults[defaultCount].entityId   == NULL) ||
      (defaults[defaultCount].entityType == NULL))
  {
    free(defaults[defaultCount].bridgeName);
    free(defaults[defaultCount].entityId);
    free(defaults[defaultCount].entityType);
    return false;
  }

  ++defaultCount;

  KT_T(KtBridge, "bridge '%s': unclaimed endpoints go to %s (%s)", bridgeName, entityId, entityType);

  return true;
}



// -----------------------------------------------------------------------------
//
// bridgeDefaultEntityGet -
//
bool bridgeDefaultEntityGet(const char* bridgeName, const char** entityIdP, const char** entityTypeP, Tenant** tenantPP)
{
  if ((defaultCount == 0) || (bridgeName == NULL))
    return false;

  for (int i = 0; i < defaultCount; i++)
  {
    if (strcmp(defaults[i].bridgeName, bridgeName) != 0)
      continue;

    if (entityIdP   != NULL)  *entityIdP   = defaults[i].entityId;
    if (entityTypeP != NULL)  *entityTypeP = defaults[i].entityType;
    if (tenantPP    != NULL)  *tenantPP    = defaults[i].tenantP;

    return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// bridgeDefaultEntityNeedsCreate -
//
bool bridgeDefaultEntityNeedsCreate(const char* bridgeName)
{
  if ((defaultCount == 0) || (bridgeName == NULL))
    return false;

  for (int i = 0; i < defaultCount; i++)
  {
    if (strcmp(defaults[i].bridgeName, bridgeName) == 0)
      return (defaults[i].created == false);
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// bridgeDefaultEntityCreated -
//
void bridgeDefaultEntityCreated(const char* bridgeName)
{
  if (bridgeName == NULL)
    return;

  for (int i = 0; i < defaultCount; i++)
  {
    if (strcmp(defaults[i].bridgeName, bridgeName) == 0)
    {
      defaults[i].created = true;
      return;
    }
  }
}
