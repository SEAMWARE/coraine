#ifndef FDSC_TOKEN_CACHE_H_
#define FDSC_TOKEN_CACHE_H_

//
// FILE            fdscTokenCache.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The tokens whose signature has been checked: a client sends the same access token with every request
// for as long as it lives (VCVerifier: 30 minutes), and checking its signature costs more than many a
// request does. A hit is the token's exact bytes; the claims are still checked on each request (exp first
// of all). Emptied whenever the key set changes - a token of a key the verifier no longer has is refused,
// as APISIX refuses it.
//
#include <stdbool.h>                                 // bool



// -----------------------------------------------------------------------------
//
// fdscTokenCacheHas - the token's signature has been checked, with the key set in use
//
extern bool fdscTokenCacheHas(const char* token, int len);



// -----------------------------------------------------------------------------
//
// fdscTokenCacheGeneration / fdscTokenCacheAdd - a token whose signature holds, checked with the keys of
// 'generation' (read BEFORE the check): not kept when the keys changed meanwhile
//
extern unsigned int fdscTokenCacheGeneration(void);
extern void         fdscTokenCacheAdd(const char* token, int len, unsigned int generation);



// -----------------------------------------------------------------------------
//
// fdscTokenCacheClear - the key set changed (and the broker stops)
//
extern void fdscTokenCacheClear(void);

#endif  // FDSC_TOKEN_CACHE_H_
