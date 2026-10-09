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
// ⚠ ONE 64-BIT SPACE, SHARED WITH EVERY OTHER REGISTRANT. corNgsild's LD_PARAM_*
// grow upward from bit 0, so the broker's own parameters and its plugins' are
// taken from the TOP (ddsSync is 63, see bridgeServiceSync.h). These were 48-51
// until corNgsild grew into them - containedBy is 48, firstN 50, offsetN 51 -
// and a route that took containedBy then took ?verbose too, as the same bit.
//
// The four parameters of /admin/log share ONE bit: they are allowed on the same routes, always together,
// and read by name - a bit each was three bits of the 64 for nothing (59-61 now GET /entities' page
// position, getEntities.h).
//
#define ADMIN_LOG_PARAMS          (1ULL << 62)



// -----------------------------------------------------------------------------
//
// adminParams - URL parameters contributed by the admin plugin
//
static CorRestParam adminParams[] =
{
  { "verbose",     ADMIN_LOG_PARAMS },
  { "debug",       ADMIN_LOG_PARAMS },
  { "info",        ADMIN_LOG_PARAMS },
  { "traceLevels", ADMIN_LOG_PARAMS },
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
