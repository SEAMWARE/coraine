//
// FILE            adminRegister.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef SWBROKER_VERSION
#define SWBROKER_VERSION "unknown"
#endif

#include "swRest/swRest.h"                       // SwRestServiceSimplified, SwRestParam, SwRestVerb, SwVerb*
#include "plugin/ApiPlugin.h"                     // ApiPlugin

#include "api/admin/adminVersion.h"               // adminGetVersion
#include "api/admin/adminHealth.h"                // adminGetHealth
#include "api/admin/adminLog.h"                   // adminGetLog, adminPutLog, adminPostLog, adminPatchLog, adminDeleteLog
#include "api/admin/adminTenants.h"               // adminGetTenants
#include "api/admin/adminPlugins.h"               // adminGetPlugins
#include "api/admin/adminMetrics.h"               // adminGetMetrics
#include "api/admin/adminSubStats.h"              // adminPostSubStatsFlush
#include "api/admin/adminTroeDump.h"              // adminGetTroeDump



// -----------------------------------------------------------------------------
//
// URL parameter bits for the admin plugin
//
#define ADMIN_PARAM_VERBOSE       (1ULL << 48)
#define ADMIN_PARAM_DEBUG         (1ULL << 49)
#define ADMIN_PARAM_INFO          (1ULL << 50)
#define ADMIN_PARAM_TRACELEVELS   (1ULL << 51)

#define ADMIN_LOG_PARAMS   (ADMIN_PARAM_VERBOSE | ADMIN_PARAM_DEBUG | ADMIN_PARAM_INFO | ADMIN_PARAM_TRACELEVELS)



// -----------------------------------------------------------------------------
//
// adminParams - URL parameters contributed by the admin plugin
//
static SwRestParam adminParams[] =
{
  { "verbose",     ADMIN_PARAM_VERBOSE     },
  { "debug",       ADMIN_PARAM_DEBUG       },
  { "info",        ADMIN_PARAM_INFO        },
  { "traceLevels", ADMIN_PARAM_TRACELEVELS },
  { NULL, 0 }
};



// -----------------------------------------------------------------------------
//
// adminServices - flat array of all services (verb included in each entry)
//
static SwRestServiceSimplified adminServices[] =
{
  // GET
  { SwVerbGet,    "/admin/version", adminGetVersion, 0,                0 },
  { SwVerbGet,    "/admin/health",  adminGetHealth,  0,                0 },
  { SwVerbGet,    "/admin/log",     adminGetLog,     0,                0 },
  { SwVerbGet,    "/admin/tenants", adminGetTenants, 0,                0 },
  { SwVerbGet,    "/admin/plugins", adminGetPlugins, 0,                0 },
  { SwVerbGet,    "/admin/metrics", adminGetMetrics, 0,                0 },
  { SwVerbGet,    "/metrics",       adminGetMetrics, 0,                0 },  // Prometheus default scrape path (metrics_path)
  { SwVerbGet,    "/admin/troe/dump", adminGetTroeDump, 0,             0 },
  // PUT
  { SwVerbPut,    "/admin/log",     adminPutLog,     ADMIN_LOG_PARAMS, 0 },
  // POST
  { SwVerbPost,   "/admin/log",             adminPostLog,             ADMIN_LOG_PARAMS, 0 },
  { SwVerbPost,   "/admin/subStats/flush",  adminPostSubStatsFlush,   0,                0 },
  // DELETE
  { SwVerbDelete, "/admin/log",     adminDeleteLog,  ADMIN_LOG_PARAMS, 0 },
  // PATCH
  { SwVerbPatch,  "/admin/log",     adminPatchLog,   ADMIN_LOG_PARAMS, 0 }
};



// -----------------------------------------------------------------------------
//
// apiRegister - called by swPluginLoadApi after dlopen
//
void apiRegister(ApiPlugin* pluginP)
{
  pluginP->alias        = "admin";
  pluginP->version      = SWBROKER_VERSION;
  pluginP->services     = adminServices;
  pluginP->serviceCount = sizeof(adminServices) / sizeof(SwRestServiceSimplified);
  pluginP->params       = adminParams;
}
