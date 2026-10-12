#ifndef FDSC_JWT_H_
#define FDSC_JWT_H_

//
// FILE            fdscJwt.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A JWT (RFC 7519) in compact JWS form, as VCVerifier issues it: the signature checked against the key
// set (fdscKeys), then the registered claims - exp, nbf, iss, aud.
//
#include <stdint.h>                                  // int64_t

#include "corAlloc/CorAlloc.h"                       // CorAlloc
#include "corTree/CorNode.h"                         // CorNode



// -----------------------------------------------------------------------------
//
// FdscJwtCheck - fdscJwtVerify's answer
//
typedef enum FdscJwtCheck
{
  FdscJwtOk           = 0,
  FdscJwtMalformed    = 1,                           // not a compact JWS, or its parts are not base64url JSON
  FdscJwtUnknownKey   = 2,                           // signed by a key the set does not have (yet: the verifier may have rotated)
  FdscJwtBadSignature = 3,
  FdscJwtBadAlg       = 7,                           // an alg other than RS256 / ES256 ("none" among them), or not the key's
  FdscJwtExpired      = 4,                           // exp passed, or nbf not reached
  FdscJwtWrongIssuer  = 5,
  FdscJwtWrongAudience = 6
} FdscJwtCheck;



// -----------------------------------------------------------------------------
//
// FdscJwtExpect - what the claims must say. NULL / 0: not checked.
//
typedef struct FdscJwtExpect
{
  const char*  issuer;                               // iss
  const char*  audience;                             // one of aud (a string or an array)
  int64_t      now;                                  // seconds since the epoch
  int          leeway;                               // seconds of clock skew allowed on exp / nbf
} FdscJwtExpect;



// -----------------------------------------------------------------------------
//
// fdscJwtVerify - the token checked; on FdscJwtOk its claims (the payload object) in *claimsPP, built in
// kaP. 'token' is not modified.
//
extern FdscJwtCheck fdscJwtVerify(const char* token, const FdscJwtExpect* expectP, CorAlloc* kaP, CorNode** claimsPP);



// -----------------------------------------------------------------------------
//
// fdscJwtCheckString - a check's name, for the 401's detail
//
extern const char* fdscJwtCheckString(FdscJwtCheck check);

#endif  // FDSC_JWT_H_
