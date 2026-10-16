#ifndef AUTH_JWT_H_
#define AUTH_JWT_H_

//
// FILE            authJwt.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A JWT (RFC 7519) in compact JWS form - an access token, as VCVerifier or an OpenID provider issues it: the
// signature checked against the key set (authKeys), then the registered claims - exp, nbf, iss, aud. Shared
// by the auth plugins (fdsc.so, oidc.so), each linking its own copy.
//
#include <stdint.h>                                  // int64_t

#include "corAlloc/CorAlloc.h"                       // CorAlloc
#include "corTree/CorNode.h"                         // CorNode



// -----------------------------------------------------------------------------
//
// AuthJwtCheck - authJwtVerify's answer
//
typedef enum AuthJwtCheck
{
  AuthJwtOk           = 0,
  AuthJwtMalformed    = 1,                           // not a compact JWS, or its parts are not base64url JSON
  AuthJwtUnknownKey   = 2,                           // signed by a key the set does not have (yet: the issuer may have rotated)
  AuthJwtBadSignature = 3,
  AuthJwtBadAlg       = 7,                           // an alg other than RS256 / ES256 ("none" among them), or not the key's
  AuthJwtExpired      = 4,                           // exp passed, or nbf not reached
  AuthJwtWrongIssuer  = 5,
  AuthJwtWrongAudience = 6
} AuthJwtCheck;



// -----------------------------------------------------------------------------
//
// AuthJwtExpect - what the claims must say. NULL / 0: not checked.
//
typedef struct AuthJwtExpect
{
  const char*  issuer;                               // iss
  const char*  audience;                             // one of aud (a string or an array)
  int64_t      now;                                  // seconds since the epoch
  int          leeway;                               // seconds of clock skew allowed on exp / nbf
} AuthJwtExpect;



// -----------------------------------------------------------------------------
//
// authJwtVerify - the token checked; on AuthJwtOk its claims (the payload object) in *claimsPP, built in
// kaP. 'token' is not modified.
//
extern AuthJwtCheck authJwtVerify(const char* token, const AuthJwtExpect* expectP, CorAlloc* kaP, CorNode** claimsPP);



// -----------------------------------------------------------------------------
//
// authJwtCheckString - a check's name, for the 401's detail
//
extern const char* authJwtCheckString(AuthJwtCheck check);

#endif  // AUTH_JWT_H_
