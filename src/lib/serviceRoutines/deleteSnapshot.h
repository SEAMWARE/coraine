#ifndef SERVICEROUTINES_DELETESNAPSHOT_H_
#define SERVICEROUTINES_DELETESNAPSHOT_H_

//
// FILE            deleteSnapshot.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// DELETE /ngsi-ld/v1/snapshots/{id} — Delete Snapshot (§ 5.16.5).
//
#include <stdbool.h>                                     // bool

extern bool deleteSnapshot(void);

#endif  // SERVICEROUTINES_DELETESNAPSHOT_H_
