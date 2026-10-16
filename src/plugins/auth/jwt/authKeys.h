#ifndef AUTH_KEYS_H_
#define AUTH_KEYS_H_

//
// FILE            authKeys.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A token issuer's signing keys - a JWK Set (RFC 7517 § 5), RSA and EC P-256 public keys - and the
// signature check of a JWS (RFC 7515) against them: RS256 and ES256 (RFC 7518 § 3.3, § 3.4), what
// VCVerifier signs with, and what OpenID providers sign with by default (RS256). The set in use is replaced
// whole (authKeysLoad) by a plugin's refresh thread while request threads check signatures against it.
//
#include <stdbool.h>                                 // bool



// -----------------------------------------------------------------------------
//
// AuthKeyCheck - authKeysVerify's answer
//
typedef enum AuthKeyCheck
{
  AuthKeyOk          = 0,
  AuthKeyUnknown     = 1,                            // no key with that kid (or, with no kid, none for the alg)
  AuthKeyBadSig      = 2,
  AuthKeyBadAlg      = 3                             // an alg other than RS256 / ES256 - or not the key's
} AuthKeyCheck;



// -----------------------------------------------------------------------------
//
// authKeysLoad - a JWK Set's text parsed and installed in place of the set in use. false: not one
// (the set in use stays) - errorBuf says why. Keys of a kind it does not know are skipped. changed (or NULL)
// is called when the set in use is replaced - not for the same text again.
//
extern bool authKeysLoad(char* jwksText, void (*changed)(void), char* errorBuf, int errorBufSize);



// -----------------------------------------------------------------------------
//
// authKeysVerify - is sig (raw, base64url-decoded) the signature of 'input' by the key 'kid' (NULL:
// any key that fits the alg)
//
extern AuthKeyCheck authKeysVerify(const char* kid, const char* alg, const char* input, int inputLen, const unsigned char* sig, int sigLen);



// -----------------------------------------------------------------------------
//
// authKeysCount - how many keys the set in use has (0: none loaded yet)
//
extern int authKeysCount(void);



// -----------------------------------------------------------------------------
//
// authKeysRelease - the set in use freed (the broker stops); changed (or NULL) called after
//
extern void authKeysRelease(void (*changed)(void));

#endif  // AUTH_KEYS_H_
