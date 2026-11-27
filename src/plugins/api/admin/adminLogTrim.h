#ifndef SRC_PLUGINS_API_ADMIN_ADMINLOGTRIM_H_
#define SRC_PLUGINS_API_ADMIN_ADMINLOGTRIM_H_

//
// FILE            adminLogTrim.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                   // bool

#include "corTree/CorNode.h"                           // CorNode



// -----------------------------------------------------------------------------
//
// adminLogTrimAdd - GET /admin/log's "file" and "trim"
//
extern void adminLogTrimAdd(CorNode* root);



// -----------------------------------------------------------------------------
//
// adminLogTrimParams - the trim limits of PUT/POST/PATCH /admin/log (trimAt, trimTo, trimKeep, trimKeepFor), checked
// and set - false: refused (the response is set), nothing changed
//
extern bool adminLogTrimParams(void);



// -----------------------------------------------------------------------------
//
// The services
//
extern bool adminPostLogTrim(void);                    // POST   /admin/log/trim
extern bool adminGetLogTrimmed(void);                  // GET    /admin/log/trimmed
extern bool adminGetLogTrimmedFile(void);              // GET    /admin/log/trimmed/{name}
extern bool adminDeleteLogTrimmedFile(void);           // DELETE /admin/log/trimmed/{name}

#endif  // SRC_PLUGINS_API_ADMIN_ADMINLOGTRIM_H_
