#ifndef MONGOC_MONGOCARRAYBEGIN_H_
#define MONGOC_MONGOCARRAYBEGIN_H_

//
// FILE            mongocArrayBegin.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// libbson 2.5 deprecates bson_append_array_begin for bson_append_array_unsafe_begin - the same function under
// a name that says the caller writes the keys "0", "1", ... itself, as every caller here does. 2.5 is the first
// release that has the new name, and the plugin is built against older ones too (Ubuntu 26.04's libbson is
// 2.2.2), so the name is chosen by the version of the headers.
//
#include <stdbool.h>                                     // bool

#include <bson/bson.h>                                   // bson_t, bson_append_array_*, BSON_CHECK_VERSION



// -----------------------------------------------------------------------------
//
// mongocArrayBegin - bson_append_array_begin, by the name the headers' libbson has for it (keyLen -1: strlen)
//
static inline bool mongocArrayBegin(bson_t* bsonP, const char* key, int keyLen, bson_t* childP)
{
#if BSON_CHECK_VERSION(2, 5, 0)
  return bson_append_array_unsafe_begin(bsonP, key, keyLen, childP);
#else
  return bson_append_array_begin(bsonP, key, keyLen, childP);
#endif
}

#endif  // MONGOC_MONGOCARRAYBEGIN_H_
