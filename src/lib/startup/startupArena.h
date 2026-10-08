#ifndef STARTUP_STARTUPARENA_H_
#define STARTUP_STARTUPARENA_H_

//
// FILE            startupArena.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//



// -----------------------------------------------------------------------------
//
// startupArena - corRest's arena and JSON parser for the main thread, before any request: what the
// stores use while the caches are loaded (mongoc allocates in corRest.kalloc). The cache items are
// cloned into malloc - this is short-lived.
//
extern void startupArena(void);

#endif  // STARTUP_STARTUPARENA_H_
