#ifndef MONGOC_MONGOCDOTESCAPE_H_
#define MONGOC_MONGOCDOTESCAPE_H_

//
// FILE            mongocDotEscape.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// MongoDB does not allow '.' in field names.
// These functions convert between regular dots and fullwidth period U+FF0E.
//
#include "kalloc/KAlloc.h"                               // KAlloc



// -----------------------------------------------------------------------------
//
// mongocEscapeDotsInKey - replace '.' with fullwidth period U+FF0E (3 bytes: EF BC 8E)
//
// Returns the original key if no dots, or a thread-local static buffer.
//
extern const char* mongocEscapeDotsInKey(const char* key);



// -----------------------------------------------------------------------------
//
// mongocUnescapeDotsInKey - replace fullwidth period U+FF0E back to '.'
//
// Returns the original key if no fullwidth periods, or a kaP-allocated copy.
//
extern const char* mongocUnescapeDotsInKey(KAlloc* kaP, const char* key);

#endif  // MONGOC_MONGOCDOTESCAPE_H_
