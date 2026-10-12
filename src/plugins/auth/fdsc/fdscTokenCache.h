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
// The tokens already checked, each with its identity: a client sends the same access token with every
// request for as long as it lives (VCVerifier: 30 minutes), and checking its signature and parsing its
// claims cost more than many a request does. A hit is the token's exact bytes, and gives the identity made
// for it (referenced); its exp / nbf are still checked at every use. Emptied whenever the key set changes -
// a token of a key the verifier no longer has is refused, as APISIX refuses it.
//
#include "fdscIdentity.h"                            // FdscIdentity



// -----------------------------------------------------------------------------
//
// fdscTokenCacheGet - the identity of a token checked already, with the key set in use - referenced (the
// caller unrefs it); NULL: not cached
//
extern FdscIdentity* fdscTokenCacheGet(const char* token, int len);



// -----------------------------------------------------------------------------
//
// fdscTokenCacheGeneration / fdscTokenCacheAdd - a token whose signature holds and its identity (the cache
// takes a reference of its own), checked with the keys of 'generation' (read BEFORE the check): not kept
// when the keys changed meanwhile
//
extern unsigned int fdscTokenCacheGeneration(void);
extern void         fdscTokenCacheAdd(const char* token, int len, FdscIdentity* idP, unsigned int generation);



// -----------------------------------------------------------------------------
//
// fdscTokenCacheClear - the key set changed (and the broker stops)
//
extern void fdscTokenCacheClear(void);

#endif  // FDSC_TOKEN_CACHE_H_
