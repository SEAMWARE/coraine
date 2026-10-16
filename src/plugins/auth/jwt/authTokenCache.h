#ifndef AUTH_TOKEN_CACHE_H_
#define AUTH_TOKEN_CACHE_H_

//
// FILE            authTokenCache.h
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
// a token of a key the issuer no longer has is refused, as APISIX refuses it. The identity is the plugin's
// own: the cache only takes and drops references on it, with the functions authTokenCacheInit gave it.
//


// -----------------------------------------------------------------------------
//
// authTokenCacheInit - how an identity is referenced and unreferenced (the last unref frees it); once, in the
// plugin's init, before any other call
//
extern void authTokenCacheInit(void (*ref)(void* identity), void (*unref)(void* identity));



// -----------------------------------------------------------------------------
//
// authTokenCacheGet - the identity of a token checked already, with the key set in use - referenced (the
// caller unrefs it); NULL: not cached
//
extern void* authTokenCacheGet(const char* token, int len);



// -----------------------------------------------------------------------------
//
// authTokenCacheGeneration / authTokenCacheAdd - a token whose signature holds and its identity (the cache
// takes a reference of its own), checked with the keys of 'generation' (read BEFORE the check): not kept
// when the keys changed meanwhile
//
extern unsigned int authTokenCacheGeneration(void);
extern void         authTokenCacheAdd(const char* token, int len, void* identity, unsigned int generation);



// -----------------------------------------------------------------------------
//
// authTokenCacheClear - the key set changed (and the broker stops)
//
extern void authTokenCacheClear(void);

#endif  // AUTH_TOKEN_CACHE_H_
