#ifndef ADMIN_PROBES_H
#define ADMIN_PROBES_H

//
// FILE            adminProbes.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                              // bool



// -----------------------------------------------------------------------------
//
// adminGetProbes - GET /admin/probes: every probe set since the start or the last DELETE, its last value
// (src/lib/probe/probe.h):
//
//   { "timescale.entitySelector": "candidates", ... }
//
extern bool adminGetProbes(void);



// -----------------------------------------------------------------------------
//
// adminGetProbe - GET /admin/probes/{name}: { "<name>": "<value>" }; 404 when it is not set
//
extern bool adminGetProbe(void);



// -----------------------------------------------------------------------------
//
// adminDeleteProbes - DELETE /admin/probes: every probe unset - 204
//
extern bool adminDeleteProbes(void);

#endif  // ADMIN_PROBES_H
