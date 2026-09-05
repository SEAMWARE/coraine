//
// FILE            adminRegister.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#ifndef CORAINE_VERSION
#define CORAINE_VERSION "unknown"
#endif

#include "corRest/corRest.h"                       // CorRestServiceSimplified, CorRestParam, CorRestVerb, CorVerb*
#include "plugin/ApiPlugin.h"                     // ApiPlugin

#include "api/admin/adminVersion.h"               // adminGetVersion
#include "api/admin/adminHealth.h"                // adminGetHealth
#include "api/admin/adminLog.h"                   // adminGetLog, adminPutLog, adminPostLog, adminPatchLog, adminDeleteLog
#include "api/admin/adminTenants.h"               // adminGetTenants
#include "api/admin/adminPlugins.h"               // adminGetPlugins
#include "api/admin/adminMetrics.h"               // adminGetMetrics
#if COR_FEATURE_SUBSCRIPTIONS
#include "api/admin/adminSubStats.h"              // adminPostSubStatsFlush
#endif
#include "api/admin/adminTroeDump.h"              // adminGetTroeDump

#include "serviceRoutines/corNotInThisBuild.h"     // corNotInThisBuild



// -----------------------------------------------------------------------------
//
// SUBS - the same argument-discarding macro the broker's own table uses
//
// A plugin's service table is subject to the feature flags exactly as the core
// one is, and it answers the same way: 501 rather than 404, from the broker's
// corNotInThisBuild - which a plugin can reach because the broker is linked
// -rdynamic and the plugin resolves against it at dlopen.
//
// This one has teeth beyond tidiness. A plugin .so LINKS with unresolved
// symbols and only fails when it is dlopen'd, so leaving the handler referenced
// here would produce a build that looks clean and a broker that dies at
// startup - which is exactly how the registrations slice found this class of
// bug, on mongoc.so.
//
#if COR_FEATURE_SUBSCRIPTIONS
#  define SUBS(handler)  handler
#else
#  define SUBS(handler)  corNotInThisBuild
#endif



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
static CorRestParam adminParams[] =
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
static CorRestServiceSimplified adminServices[] =
{
  // GET
  { CorVerbGet,    "/admin/version", adminGetVersion, 0,                0 },
  { CorVerbGet,    "/admin/health",  adminGetHealth,  0,                0 },
  { CorVerbGet,    "/admin/log",     adminGetLog,     0,                0 },
  { CorVerbGet,    "/admin/tenants", adminGetTenants, 0,                0 },
  { CorVerbGet,    "/admin/plugins", adminGetPlugins, 0,                0 },
  { CorVerbGet,    "/admin/metrics", adminGetMetrics, 0,                0 },
  { CorVerbGet,    "/metrics",       adminGetMetrics, 0,                0 },  // Prometheus default scrape path (metrics_path)
  { CorVerbGet,    "/admin/troe/dump", adminGetTroeDump, 0,             0 },
  // PUT
  { CorVerbPut,    "/admin/log",     adminPutLog,     ADMIN_LOG_PARAMS, 0 },
  // POST
  { CorVerbPost,   "/admin/log",             adminPostLog,             ADMIN_LOG_PARAMS, 0 },
  { CorVerbPost,   "/admin/subStats/flush",  SUBS(adminPostSubStatsFlush), 0,            0 },
  // DELETE
  { CorVerbDelete, "/admin/log",     adminDeleteLog,  ADMIN_LOG_PARAMS, 0 },
  // PATCH
  { CorVerbPatch,  "/admin/log",     adminPatchLog,   ADMIN_LOG_PARAMS, 0 }
};



// -----------------------------------------------------------------------------
//
// apiRegister - called by corPluginLoadApi after dlopen
//
void apiRegister(ApiPlugin* pluginP)
{
  pluginP->alias        = "admin";
  pluginP->version      = CORAINE_VERSION;
  pluginP->services     = adminServices;
  pluginP->serviceCount = sizeof(adminServices) / sizeof(CorRestServiceSimplified);
  pluginP->params       = adminParams;
}
