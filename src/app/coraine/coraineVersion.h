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
// coraine product version. On main: the next release with "-dev" ("0.6.0-dev") - main is never a
// release. A release branch sets the plain version ("0.6.0"), and its tag (v0.6.0) publishes it.
// The Debian packages take the part before '-' (packaging/deb/version.sh): main's builds are
// 0.6.0~git<date>.<sha>, after 0.5.0 and before 0.6.0.
// Consumed in:
//   - User-Agent on outgoing HTTP (notifications, distops, @context fetches)
//   - GET /info/sourceIdentity (contextSourceVersion field, § 5.2.40)
//   - GET /version (broker product/version handshake)
//
#define CORAINE_VERSION "0.6.0-dev"

#endif  // SRC_APP_CORAINE_CORAINEVERSION_H_
