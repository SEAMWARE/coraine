#ifndef SERVICEROUTINES_GETSNAPSHOT_H_
#define SERVICEROUTINES_GETSNAPSHOT_H_

//
// FILE            getSnapshot.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/snapshots/{id} — Retrieve Snapshot Status (§ 5.16.3).
//
#include <stdbool.h>                                     // bool

extern bool getSnapshot(void);

#endif  // SERVICEROUTINES_GETSNAPSHOT_H_
