#ifndef SRC_APP_CORAINE_CORAINEVERSION_H_
#define SRC_APP_CORAINE_CORAINEVERSION_H_

//
// FILE            coraineVersion.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// coraine product version. Bumped on user-visible behaviour changes
// (new routes, spec coverage, response shape, default config).
// Consumed in:
//   - User-Agent on outgoing HTTP (notifications, distops, @context fetches)
//   - GET /info/sourceIdentity (contextSourceVersion field, § 5.2.40)
//   - GET /version (broker product/version handshake)
//
#define CORAINE_VERSION "0.4.0"

#endif  // SRC_APP_CORAINE_CORAINEVERSION_H_
