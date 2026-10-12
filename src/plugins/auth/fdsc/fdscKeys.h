#ifndef FDSC_KEYS_H_
#define FDSC_KEYS_H_

//
// FILE            fdscKeys.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The verifier's signing keys - a JWK Set (RFC 7517 § 5), RSA and EC P-256 public keys - and the
// signature check of a JWS (RFC 7515) against them: RS256 and ES256 (RFC 7518 § 3.3, § 3.4), what
// VCVerifier signs with. The set in use is replaced whole (fdscKeysInstall) by the refresh thread while
// request threads check signatures against it.
//
#include <stdbool.h>                                 // bool



// -----------------------------------------------------------------------------
//
// FdscKeyCheck - fdscKeysVerify's answer
//
typedef enum FdscKeyCheck
{
  FdscKeyOk          = 0,
  FdscKeyUnknown     = 1,                            // no key with that kid (or, with no kid, none for the alg)
  FdscKeyBadSig      = 2,
  FdscKeyBadAlg      = 3                             // an alg other than RS256 / ES256 - or not the key's
} FdscKeyCheck;



// -----------------------------------------------------------------------------
//
// fdscKeysLoad - a JWK Set's text parsed and installed in place of the set in use. false: not one
// (the set in use stays) - errorBuf says why. Keys of a kind it does not know are skipped.
//
extern bool fdscKeysLoad(char* jwksText, char* errorBuf, int errorBufSize);



// -----------------------------------------------------------------------------
//
// fdscKeysVerify - is sig (raw, base64url-decoded) the signature of 'input' by the key 'kid' (NULL:
// any key that fits the alg)
//
extern FdscKeyCheck fdscKeysVerify(const char* kid, const char* alg, const char* input, int inputLen, const unsigned char* sig, int sigLen);



// -----------------------------------------------------------------------------
//
// fdscKeysCount - how many keys the set in use has (0: none loaded yet)
//
extern int fdscKeysCount(void);



// -----------------------------------------------------------------------------
//
// fdscKeysRelease - the set in use freed (the broker stops)
//
extern void fdscKeysRelease(void);

#endif  // FDSC_KEYS_H_
