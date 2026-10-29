#ifndef PROBE_PROBE_H_
#define PROBE_PROBE_H_

//
// FILE            probe.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Probes - what the code decided, readable from outside: GET /admin/probes (the admin API plugin).
//
// A probe is a name and the last value the code set it to. It is there for the functional tests: a test that
// needs to know HOW the broker did something (which plan a query took, which path a request went down) asks the
// probe, not the log - the traces are compiled out of a release build, and a test reading them fails on one.
//
// Setting a probe is one atomic store - no allocation, no lock, no formatting - so a probe stays in the release
// build. It is compiled out with COR_FEATURE_PROBES=OFF: PROBE_SET is then nothing (its value still type-checked).
//
// A probe's value is a string that lives as long as the broker - a literal. The last set wins, whichever thread
// set it: a test reads a probe after the one request it is about.
//
#include <stdatomic.h>                             // _Atomic



// -----------------------------------------------------------------------------
//
// Probe - one probe; a static of the code that sets it, registered at its first set
//
typedef struct Probe
{
  const char*              name;
  _Atomic(const char*)     value;                  // NULL: not set since the start or the last probeReset
  atomic_flag              registered;             // set by the first probeSet, that one adds it to the list
  struct Probe*            next;
} Probe;



#if COR_FEATURE_PROBES

// -----------------------------------------------------------------------------
//
// PROBE_SET - the probe 'name' is now 'value' (a string literal, or any string that never goes away)
//
#define PROBE_SET(name, value)                                    \
  do                                                              \
  {                                                               \
    static Probe probe_ = { name, NULL, ATOMIC_FLAG_INIT, NULL }; \
    probeSet(&probe_, value);                                     \
  } while (0)



// -----------------------------------------------------------------------------
//
// probeSet - see PROBE_SET
//
extern void probeSet(Probe* probeP, const char* value);



// -----------------------------------------------------------------------------
//
// probeList - the probes set so far, in no particular order - NULL ends the list; a probe whose value is NULL
// has not been set since the last probeReset
//
extern Probe* probeList(void);



// -----------------------------------------------------------------------------
//
// probeReset - every probe unset (DELETE /admin/probes)
//
extern void probeReset(void);

#else

#define PROBE_SET(name, value)  do { if (0) { const char* v_ = (value); (void) v_; } } while (0)

#endif

#endif  // PROBE_PROBE_H_
