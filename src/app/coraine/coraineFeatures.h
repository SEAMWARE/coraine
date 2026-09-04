#ifndef SRC_APP_CORAINE_CORAINEFEATURES_H_
#define SRC_APP_CORAINE_CORAINEFEATURES_H_

//
// FILE            coraineFeatures.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                 // bool



// -----------------------------------------------------------------------------
//
// CoraineFeature - one optional capability and whether this build has it
//
// 'name' is the CMake option's suffix verbatim (COR_FEATURE_REGISTRATIONS ->
// "REGISTRATIONS"), so build flag, compile define, --version output, /version
// response and the functional-test REQUIRE_FEATURE marker are all the same
// word. One spelling means no mapping table to get out of step.
//
typedef struct CoraineFeature
{
  const char* name;
  bool        on;
} CoraineFeature;



// -----------------------------------------------------------------------------
//
// coraineFeatures - the build's feature set, terminated by a NULL name
//
extern const CoraineFeature coraineFeatures[];

#endif  // SRC_APP_CORAINE_CORAINEFEATURES_H_
