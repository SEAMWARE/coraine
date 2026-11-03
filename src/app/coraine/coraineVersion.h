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
// coraine product version. On main: the LAST release with "+dev" ("0.6.0+dev", semver build
// metadata) - after 0.6.0, without naming the next release before it is decided. The pull request
// that prepares a release sets the plain version ("0.7.0" or "1.0.0"), and its tag (v0.7.0), on
// main, publishes it. The Debian packages of a "+dev" build are 0.6.0+git<date>.<sha>
// (packaging/deb/version.sh), after 0.6.0 and before the next release; its images are tagged
// 0.6.0-post-<date>-<commit> (an image tag cannot hold '+').
// Consumed in:
//   - User-Agent on outgoing HTTP (notifications, distops, @context fetches)
//   - GET /info/sourceIdentity (contextSourceVersion field, § 5.2.40)
//   - GET /version (broker product/version handshake)
//
#define CORAINE_VERSION "0.6.2"

#endif  // SRC_APP_CORAINE_CORAINEVERSION_H_
