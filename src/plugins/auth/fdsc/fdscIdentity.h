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
// Who sent a request: the claims of its access token, and the credential VCVerifier put in it. An
// FdscIdentity is made once per token and SHARED - by the token cache and by every request that brings the
// same token - so it is never written once filled, and counted (fdscIdentityRef / Unref). What a request
// keeps of its own is its FdscHandle: the identity and the request's own state (AuthDriver's identity).
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

  long long  exp;                                    // the token's exp, nbf (0: none) - checked at every use of a cached identity
  long long  nbf;
  int        refs;
} FdscIdentity;



// -----------------------------------------------------------------------------
//
// FdscHandle - one request's (or one subscription owner's): the identity, and what the request decides
// for itself
//
typedef struct FdscHandle
{
  FdscIdentity*  idP;                                // NULL: no one (--fdscAnonymous)
  bool           entityFree;                         // --fdscNgsild: a policy allows the request whatever its entities (set by authorize)
} FdscHandle;



// -----------------------------------------------------------------------------
//
// fdscIdentityNew / fdscIdentityFree
//
extern FdscIdentity* fdscIdentityNew(void);              // one reference
extern void          fdscIdentityFree(FdscIdentity* idP);   // = Unref
extern void          fdscIdentityRef(FdscIdentity* idP);
extern void          fdscIdentityUnref(FdscIdentity* idP);   // the last frees it



// -----------------------------------------------------------------------------
//
// fdscHandleNew / fdscHandleFree - a handle takes the caller's reference to idP (NULL allowed); freeing it
// lets that reference go
//
extern FdscHandle* fdscHandleNew(FdscIdentity* idP);
extern void        fdscHandleFree(FdscHandle* hP);



// -----------------------------------------------------------------------------
//
// fdscIdentityFill - the claims of a verified token, and its credentials found
//
extern void fdscIdentityFill(FdscIdentity* idP, CorNode* claimsP);

#endif  // FDSC_IDENTITY_H_
