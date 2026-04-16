//
// FILE            getCsourceRegistrations.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/csourceRegistrations  (NGSI-LD § 5.10.2 / § 6.8.3.2)
//
// First-cut behaviour: list all registrations for the tenant, paginated
// via limit/offset/count. The Discovery filter (type / id / idPattern /
// attrs / q / geoQ family / scopeQ / csf) is intentionally NOT
// implemented — see project_csr_discovery_deferred memory. Any of those
// filter params present on the request gets a 501 Not Implemented so
// clients don't silently receive unfiltered results.
//
#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp

#include "swRest/SwRestState.h"                      // swRest
#include "swNgsild/swNgsild.h"                       // ldError, LD_ERROR_*, swNgsild, ldContextResolve

#include "db/DbDriver.h"                             // db, DB_OK
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/getCsourceRegistrations.h" // Own interface



// -----------------------------------------------------------------------------
//
// paramSupported - true for the URL params we actually honor on this route
//
static bool paramSupported(const char* key)
{
  if (key == NULL)                       return true;  // ignore malformed
  if (strcmp(key, "limit")     == 0)     return true;
  if (strcmp(key, "offset")    == 0)     return true;
  if (strcmp(key, "count")     == 0)     return true;
  return false;
}



// -----------------------------------------------------------------------------
//
// getCsourceRegistrations -
//
bool getCsourceRegistrations(void)
{
  // 501 if any Discovery filter URL param is supplied (§ 5.10.2 not implemented)
  for (int i = 0; i < swRest.in.uriParamCount; i++)
  {
    const char* key = swRest.in.uriParamV[i].key;
    if (paramSupported(key) == false)
    {
      ldError(501, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
              "Context Source Registration discovery filter '%s' is not implemented; only limit/offset/count are honored",
              key);
      return true;
    }
  }

  if (db.registrationQuery == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented", "registration CRUD not supported by this DB plugin");
    return true;
  }

  KjNode* arrayP = NULL;
  int     r      = db.registrationQuery((Tenant*) swNgsild.tenantP, swNgsild.limit, swNgsild.offset, &arrayP);

  if (r != DB_OK)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "database error querying registrations");
    return true;
  }

  ldContextResolve();

  swNgsild.rawResponse    = true;
  swRest.out.responseTree = arrayP;
  return true;
}
