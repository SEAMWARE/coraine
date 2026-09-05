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



// -----------------------------------------------------------------------------
//
// CorainePlugin - one plugin the build that produced this binary also produced
//
// 'kind' is the plugin category as the install lays it out ("currentState",
// "temporal", "api"), 'name' the short alias the broker loads it by.
//
typedef struct CorainePlugin
{
  const char* kind;
  const char* name;
} CorainePlugin;



// -----------------------------------------------------------------------------
//
// coraineBuiltPlugins - the plugins this build produces, terminated by NULL kind
//
// "Produces", not "has installed": they are separate .so files, loaded at run
// time from a directory this binary does not own, so a build can carry a plugin
// that is not installed and an install can hold a plugin from another build.
// What this list answers is the question the binary CAN answer - did the cmake
// run that produced me also produce that plugin - which is the half an operator
// cannot get from `ls`.
//
extern const CorainePlugin coraineBuiltPlugins[];

#endif  // SRC_APP_CORAINE_CORAINEFEATURES_H_
