#ifndef HA_HAINIT_H_
#define HA_HAINIT_H_

//
// FILE            haInit.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdbool.h>                                     // bool



// -----------------------------------------------------------------------------
//
// haChannel - the --ha option: how the broker instances tell each other
//
// NULL or empty: HA is off, and this broker's caches are its own business.
// "mongo":       the database's change feed, produced by the DB plugin.
// <ip:port>:     the haaux server (not implemented yet).
//
extern char* haChannel;



// -----------------------------------------------------------------------------
//
// haInit - start the HA channel named by --ha
//
// Called after the DB plugin is up (which channels are possible depends on it)
// and BEFORE the caches are loaded from the database. That order is what closes
// the startup gap: a change made by another instance between "we read the
// database" and "we started listening" would otherwise be missed for the
// lifetime of the process. Listening first means the two overlap instead, and an
// event for something the load also brings in is applied twice - which costs a
// re-read and changes nothing.
//
// Refuses to start rather than run with a sync that silently never fires.
//
extern bool haInit(void);



// -----------------------------------------------------------------------------
//
// haApplyEnable - the caches are loaded; events may now be applied
//
extern void haApplyEnable(void);



// -----------------------------------------------------------------------------
//
// haApplyWait - block until the caches are loaded
//
// ⚠️ EVERY CHANNEL CALLS THIS before it does anything with an event - before it
// even resolves which tenant the event belongs to, because resolving one CREATES
// it, and a tenant invented while the startup load is walking the tenant list
// would get its caches filled with that one item and nothing else.
//
// It blocks only during startup, and only if an event arrives that early.
//
extern void haApplyWait(void);

#endif  // HA_HAINIT_H_
