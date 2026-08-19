#ifndef ADMIN_ADMINLOG_H_
#define ADMIN_ADMINLOG_H_

//
// FILE            adminLog.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                              // bool



// -----------------------------------------------------------------------------
//
// adminGetLog -
//
extern bool adminGetLog(void);



// -----------------------------------------------------------------------------
//
// adminPutLog - replace log settings (trace levels replaced entirely)
//
extern bool adminPutLog(void);



// -----------------------------------------------------------------------------
//
// adminPostLog - add to log settings (trace levels added)
//
extern bool adminPostLog(void);



// -----------------------------------------------------------------------------
//
// adminPatchLog - alias for adminPostLog (add trace levels)
//
extern bool adminPatchLog(void);



// -----------------------------------------------------------------------------
//
// adminDeleteLog - remove log settings (trace levels removed, flags turned off)
//
extern bool adminDeleteLog(void);

#endif  // ADMIN_ADMINLOG_H_
