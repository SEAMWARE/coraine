#ifndef SERVICEROUTINES_CLONESNAPSHOT_H_
#define SERVICEROUTINES_CLONESNAPSHOT_H_

//
// FILE            cloneSnapshot.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /ngsi-ld/v1/snapshots/{id}/clone — Clone Snapshot (§ 5.16.2).
//
#include <stdbool.h>                                     // bool

extern bool cloneSnapshot(void);

#endif  // SERVICEROUTINES_CLONESNAPSHOT_H_
