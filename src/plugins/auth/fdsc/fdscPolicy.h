#ifndef FDSC_POLICY_H_
#define FDSC_POLICY_H_

//
// FILE            fdscPolicy.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The ODRL policies of the ODRL-PAP, evaluated as the Data Space Connector evaluates them - the Rego the
// PAP generates from each one, run by OPA on what APISIX sends it (doc/authorization.md § 5):
//
//   - a request is allowed when ONE policy allows it, denied when none does (and when there is none);
//   - a policy allows when ALL its rules hold - the rules of ALL its permissions together (the PAP puts
//     them in one rule set: two permissions are an AND, not an OR); prohibitions, obligations and duties
//     are ignored, as the PAP ignores them;
//   - a rule on a value the request does not have is not true - nor its negation, nor a logical
//     constraint that holds it (Rego's undefined).
//
#include <stdbool.h>                                 // bool

#include "plugin/AuthDriver.h"                       // AuthRequest, AuthAnswer, AuthVerdict
#include "fdscIdentity.h"                            // FdscIdentity



// -----------------------------------------------------------------------------
//
// fdscPolicyOrganizationSet - the provider's organisation DID - the PAP's GENERAL_ORGANIZATION_DID - which
// vc:role looks for in a credential's roles
//
extern void fdscPolicyOrganizationSet(const char* did);



// -----------------------------------------------------------------------------
//
// fdscPolicySetBegin / fdscPolicySetAdd / fdscPolicySetCommit - a set built from one or more texts (the PAP
// pages its policies) and installed, in place of the set in use, once complete. A text: the PAP's GET
// /policy answer (an array of { id, odrl:uid, odrl: "<the policy, as a string>" }), an array of ODRL
// policies, one policy, or an @graph of them. A policy that cannot be read is skipped, and said in the log
// - as the PAP refuses it. Add answers how many policies the text held, -1: none
// of the forms fdscPolicyLoad takes; *itemsP: how many items it had (a page with none is the last page).
// Begin drops a set begun and not committed.
//
extern void fdscPolicySetBegin(void);
extern int  fdscPolicySetAdd(char* text, int* itemsP, char* errorBuf, int errorBufSize);
extern void fdscPolicySetCommit(void);



// -----------------------------------------------------------------------------
//
// fdscPolicyDecide - AuthAllow, AuthDeny (no policy allows), AuthUnavailable (OPA's error cases: a
// credential with two different role lists for the organisation)
//
extern AuthVerdict fdscPolicyDecide(const AuthRequest* reqP, FdscIdentity* idP, AuthAnswer* answerP);



// -----------------------------------------------------------------------------
//
// fdscPolicyNgsildSet / fdscPolicyEntity - --fdscNgsild: ngsi-ld:entityType is the entity's - its stored,
// expanded types, compared with the policy's type expanded with the broker's default context - and is
// decided by fdscPolicyEntity (AuthDriver.entity) for each entity of an operation the broker checks entity
// by entity. Without --fdscNgsild fdscPolicyEntity allows everything: fdscPolicyDecide has decided, as
// the Data Space Connector does.
//
extern void        fdscPolicyNgsildSet(bool on);
extern AuthVerdict fdscPolicyEntity(const AuthRequest* reqP, FdscIdentity* idP, const AuthEntity* entityP, AuthAnswer* answerP);



// -----------------------------------------------------------------------------
//
// fdscPolicyLoaded / fdscPolicyCount / fdscPolicyRelease
//
extern bool fdscPolicyLoaded(void);                  // a set has been installed - even an empty one
extern int  fdscPolicyCount(void);
extern void fdscPolicyRelease(void);

#endif  // FDSC_POLICY_H_
