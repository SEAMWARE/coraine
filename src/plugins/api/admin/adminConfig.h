#ifndef ADMIN_CONFIG_H
#define ADMIN_CONFIG_H

//
// FILE            adminConfig.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                              // bool



// -----------------------------------------------------------------------------
//
// adminGetConfig - GET /admin/config: every option the broker runs with - the broker's and its plugins' - in
// the order they were declared:
//
//   { "options": [ { "name": "--port", "short": "-port", "type": "int", "value": 1026, "default": 1026,
//                    "from": "longOption", "env": "CORAINE_PORT", "section": "...", "description": "..." },
//                  ... ] }
//
// "from": default, configFile, env, shortOption, longOption. An option marked secret (corArgsSecret: a password,
// a URI that may carry one) has "secret": true and its value and default "***" when set - never the value.
// What is compiled in is GET /build's.
//
extern bool adminGetConfig(void);

#endif  // ADMIN_CONFIG_H
