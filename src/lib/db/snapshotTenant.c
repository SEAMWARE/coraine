//
// FILE            snapshotTenant.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Snap-tenant lifecycle helpers — see header.
//
#include <stdbool.h>                                     // bool
#include <stdio.h>                                       // snprintf
#include <stdlib.h>                                      // malloc, free
#include <string.h>                                      // strlen, strcpy, memset

#include "ktrace/kTrace.h"                               // KT_I

#include "db/DbDriver.h"                                 // db, DB_OK
#include "db/Tenant.h"                                   // Tenant, tenant0

#include "db/snapshotTenant.h"                           // Own interface


//
// nameSuffix - format "-_snap_<seq>" into the supplied buffer.
//
// Leading dash on the suffix matters: it lets the dbName always look
// like "<prefix>-..." so existing tenant-cleanup tooling (swDbDrop's
// "n===prefix || n.startsWith(prefix+'-')" filter) catches snap-tenant
// DBs without modification.
//
static void nameSuffix(char* buf, size_t bufLen, int snapSeq)
{
  snprintf(buf, bufLen, "-_snap_%x", snapSeq);
}



Tenant* snapshotTenantCreate(Tenant* origP, int snapSeq)
{
  if (origP == NULL)
    return NULL;

  char suffix[32];
  nameSuffix(suffix, sizeof(suffix), snapSeq);

  Tenant* tP = (Tenant*) malloc(sizeof(Tenant));
  if (tP == NULL)
    return NULL;
  memset(tP, 0, sizeof(*tP));

  // tenant.name = origName + suffix. For the default tenant
  // (origName = "") this leaves a leading '-_snap_<seq>' which is
  // unusual but harmless — snap-tenants are never looked up by name.
  size_t origNameLen = strlen(origP->name);
  size_t suffixLen   = strlen(suffix);
  if (origNameLen + suffixLen >= sizeof(tP->name))
  {
    free(tP);
    return NULL;
  }
  memcpy(tP->name, origP->name, origNameLen);
  memcpy(tP->name + origNameLen, suffix, suffixLen);
  tP->name[origNameLen + suffixLen] = 0;

  // tenant.dbName = origDbName + suffix. Always yields a name of the
  // form "<prefix>-..." (default tenant's dbName is "<prefix>", so the
  // suffix supplies the dash).
  size_t origDbLen = strlen(origP->dbName);
  if (origDbLen + suffixLen >= sizeof(tP->dbName))
  {
    free(tP);
    return NULL;
  }
  memcpy(tP->dbName, origP->dbName, origDbLen);
  memcpy(tP->dbName + origDbLen, suffix, suffixLen);
  tP->dbName[origDbLen + suffixLen] = 0;

  tP->initialized = false;
  // No subCacheP / pernotCacheP / regCacheP / regSubCacheP / entityMapStoreP
  // / snapshotCacheP — snap-tenants don't participate in those flows.

  if (db.tenantSetup != NULL)
  {
    if (db.tenantSetup(tP) != DB_OK)
    {
      free(tP);
      return NULL;
    }
  }
  tP->initialized = true;

  KT_I("snapshotTenant: created '%s' (db: '%s')", tP->name, tP->dbName);
  return tP;
}



void snapshotTenantDestroy(Tenant* snapTenantP)
{
  if (snapTenantP == NULL)
    return;
  free(snapTenantP);
}
