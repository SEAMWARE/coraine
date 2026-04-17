#ifndef DB_TENANT_H_
#define DB_TENANT_H_

//
// FILE            Tenant.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdbool.h>                                     // bool



// -----------------------------------------------------------------------------
//
// Tenant - tenant descriptor with pre-built database name
//
typedef struct Tenant
{
  char            name[64];       // tenant name (empty string for default)
  char            dbName[128];    // "prefix" or "prefix-tenantname"
  bool            initialized;    // true after DB setup (indexes created)
  void*           pluginData;     // opaque, owned by the DB plugin
  void*           subCacheP;      // subscription cache (LdSubCache*), owned by broker
  void*           pernotCacheP;   // periodic notification cache (LdPernotCache*), owned by broker
  void*           regCacheP;      // registration cache (LdRegCache*), owned by broker
  struct Tenant*  next;           // linked list
} Tenant;



// -----------------------------------------------------------------------------
//
// tenant0 - default tenant (no NGSILD-Tenant header)
//
extern Tenant  tenant0;
extern Tenant* tenantList;



// -----------------------------------------------------------------------------
//
// tenantInit - initialize default tenant with DB prefix
//
extern void    tenantInit(const char* dbPrefix);



// -----------------------------------------------------------------------------
//
// tenantLookup - find a tenant by name (NULL/empty => default)
//
extern Tenant* tenantLookup(const char* name);



// -----------------------------------------------------------------------------
//
// tenantGetOrCreate - find or allocate a new tenant
//
extern Tenant* tenantGetOrCreate(const char* name);



// -----------------------------------------------------------------------------
//
// tenantFromRequest - resolve tenant from NGSILD-Tenant request header
//
// autoCreate == true:  write operations (POST) — create tenant if new
// autoCreate == false: read operations (GET)  — return NULL + 404 if unknown
//
extern Tenant* tenantFromRequest(bool autoCreate);



// -----------------------------------------------------------------------------
//
// tenantPreServiceHook - swRest preServiceHook for tenant resolution
//
extern bool tenantPreServiceHook(void);



// -----------------------------------------------------------------------------
//
// tenantSubCacheReload - load subscriptions from DB into cache for all tenants
//
// Called once at startup after db.init(). For persistent DB plugins (mongoc),
// this restores the subscription cache from the previous session.
//
extern void tenantSubCacheReload(void);



// -----------------------------------------------------------------------------
//
// tenantRegCacheReload - load registrations from DB into cache for all tenants
//
// Called once at startup after db.init(). For persistent DB plugins (mongoc),
// this restores the registration cache from the previous session.
//
extern void tenantRegCacheReload(void);

#endif  // DB_TENANT_H_
