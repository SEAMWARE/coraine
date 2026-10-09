#ifndef SHARED_DBPLUGINABI_H_
#define SHARED_DBPLUGINABI_H_

//
// FILE            dbPluginAbi.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The DB / TRoE plugin interface stamp, the plugin's side (doc/plugin-architecture.md, "The DB plugin
// interface stamp"). Compiled into every DB and TRoE plugin - mongoc, none, timescale, and corDB's
// corDB.so, ramDB.so and troe/ramDB.so.
//
// A plugin and its broker share structs by layout (DbDriver, DbQueryFilter, Tenant, TroeDriver, ...).
// Built against other headers, the plugin reads and writes them at the wrong offsets: memory is
// corrupted, so a mismatch is refused, never tolerated. Both directions:
//
//   - the broker looks up the plugin's dbPluginAbi before it calls the register function, and refuses
//     a plugin without it or with another stamp (pluginLoader.c)
//   - the plugin's register function calls dbPluginAbiBrokerCheck first, which refuses a broker without
//     coraineDbAbi - a broker older than the check, which would load the plugin unchecked - or with
//     another stamp
//



// -----------------------------------------------------------------------------
//
// dbPluginAbi - this plugin's stamp: tools/dbAbiStamp.sh run on the headers it was built against
//
// Exported for the broker's dlsym. A char array, so the symbol's address IS the string.
//
extern const char dbPluginAbi[];



// -----------------------------------------------------------------------------
//
// dbPluginAbiBrokerCheck - the broker's stamp (coraineDbAbi) against this plugin's; exit(1) on a mismatch
//
// The first thing every dbRegister / troeRegister does. kind is "DB" or "TRoE", for the message.
//
// It exits rather than returning a failure: the register functions return nothing, and a broker built
// before the check - the one case where the broker has not already compared the stamps - would ignore
// whatever came back and go on with the plugin loaded.
//
extern void dbPluginAbiBrokerCheck(const char* kind);

#endif  // SHARED_DBPLUGINABI_H_
