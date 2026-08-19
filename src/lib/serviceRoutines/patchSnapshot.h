#ifndef SERVICEROUTINES_PATCHSNAPSHOT_H_
#define SERVICEROUTINES_PATCHSNAPSHOT_H_

//
// FILE            patchSnapshot.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// PATCH /ngsi-ld/v1/snapshots/{id} — Update Snapshot Status (§ 5.16.4).
//
#include <stdbool.h>                                     // bool

extern bool patchSnapshot(void);

#endif  // SERVICEROUTINES_PATCHSNAPSHOT_H_
