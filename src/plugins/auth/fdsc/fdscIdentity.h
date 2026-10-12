#ifndef FDSC_IDENTITY_H_
#define FDSC_IDENTITY_H_

//
// FILE            fdscIdentity.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Who sent a request: the claims of its access token, and the credential VCVerifier put in it - one
// per request, freed when the request is over (AuthDriver.release).
//
#include "corAlloc/CorAlloc.h"                       // CorAlloc
#include "corTree/CorNode.h"                         // CorNode



// -----------------------------------------------------------------------------
//
// FdscIdentity
//
typedef struct FdscIdentity
{
  CorAlloc   ka;                                     // the claims live here
  char       buf[4096];

  CorNode*   claimsP;                                // the token's payload
  CorNode*   credentialV[8];                         // the Verifiable Credentials it carries (verifiableCredential, or each of verifiablePresentation)
  int        credentials;

  bool       entityFree;                             // --fdscNgsild: a policy allows the request whatever its entities (set by authorize)
} FdscIdentity;



// -----------------------------------------------------------------------------
//
// fdscIdentityNew / fdscIdentityFree
//
extern FdscIdentity* fdscIdentityNew(void);
extern void          fdscIdentityFree(FdscIdentity* idP);



// -----------------------------------------------------------------------------
//
// fdscIdentityFill - the claims of a verified token, and its credentials found
//
extern void fdscIdentityFill(FdscIdentity* idP, CorNode* claimsP);

#endif  // FDSC_IDENTITY_H_
