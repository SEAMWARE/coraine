#ifndef SRC_LIB_SERVICEROUTINES_REGCONFLICTCHECK_H_
#define SRC_LIB_SERVICEROUTINES_REGCONFLICTCHECK_H_
//
// FILE            regConflictCheck.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kjson/KjNode.h"                             // KjNode
#include "kalloc/KAlloc.h"                            // KAlloc
#include "corNgsild/LdRegCache.h"                      // LdRegMode



// -----------------------------------------------------------------------------
//
// regModeOf - resolve a registration document's mode (default = inclusive, § 5.2.9)
//
extern LdRegMode regModeOf(KjNode* regP);



// -----------------------------------------------------------------------------
//
// regConflictCheck - run the § 5.9.2 / § 12.2.3.4 creation/update conflict checks
//
// Shared by the Create (POST) and Update (PATCH) registration paths so the same
// exclusive/redirect overlap rule is enforced on every write — a PATCH that
// turns a registration exclusive must collide with an overlapping registration
// or a locally-stored entity exactly as a direct create would.
//
// 'newMode' is the resulting mode of 'regP'. 'selfRegId' is the id of the
// registration being written; its own cached entry is skipped so an in-place
// PATCH doesn't conflict with the pre-update copy of itself (pass the new id on
// create — it isn't cached yet, so it's harmless).
//
// Returns true if a conflict was found AND ldError (409) was raised — the caller
// must then just return. Returns false when there is no conflict.
//
extern bool regConflictCheck(KjNode* regP, LdRegMode newMode, const char* selfRegId, KAlloc* allocP);

#endif  // SRC_LIB_SERVICEROUTINES_REGCONFLICTCHECK_H_
