#ifndef HA_HAEVENTAPPLY_H_
#define HA_HAEVENTAPPLY_H_

//
// FILE            haEventApply.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                     // bool

#include "ha/HaEvent.h"                                  // HaEvent



// -----------------------------------------------------------------------------
//
// haEventApply - bring our caches in line with what another instance did
//
// Channel-agnostic: every HA channel builds an HaEvent and calls this. Safe to
// call from a channel's own thread - the per-thread state it needs is brought up
// on first use.
//
extern bool haEventApply(HaEvent* eventP);

#endif  // HA_HAEVENTAPPLY_H_
