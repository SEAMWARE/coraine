#ifndef SERVICEROUTINES_PURGESNAPSHOTS_H_
#define SERVICEROUTINES_PURGESNAPSHOTS_H_

//
// FILE            purgeSnapshots.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// DELETE /ngsi-ld/v1/snapshots — Purge Snapshots (§ 5.16.7).
//
#include <stdbool.h>                                     // bool

extern bool purgeSnapshots(void);

#endif  // SERVICEROUTINES_PURGESNAPSHOTS_H_
