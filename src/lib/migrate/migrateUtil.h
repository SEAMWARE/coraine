#ifndef MIGRATE_MIGRATEUTIL_H_
#define MIGRATE_MIGRATEUTIL_H_

//
// FILE            migrateUtil.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdint.h>                                       // int64_t
#include <stdbool.h>                                      // bool

#include "corTree/CorNode.h"                              // CorNode

#include "corNgsild/ldTermId.h"                           // CorTerm

#include "migrate/MigrateState.h"                         // MigrateState, MigrateKind



// -----------------------------------------------------------------------------
//
// migrateTermCheck - is 'term' an IRI or an NGSI-LD core term?
//
// The stream is expanded NGSI-LD, whatever its source: anything else is a stream that was not expanded.
//
// Reports the refusal (migrateFail) and returns false.
//
extern bool migrateTermCheck(MigrateState* msP, MigrateKind kind, const char* term, const char* what);



// -----------------------------------------------------------------------------
//
// migrateEntityTermsCheck - migrateTermCheck on every name of an Entity that needs expanding
//
// The type(s), the Attribute names, the Sub-Attribute names (at every level) and a VocabProperty's
// vocab. NOT the member names of a compound value: those are the application's own JSON.
//
extern bool migrateEntityTermsCheck(MigrateState* msP, MigrateKind kind, CorNode* entityP);



// -----------------------------------------------------------------------------
//
// migrateTimeTake - take a DateTime member (by term) out of an object and return it as epoch ns
//
// 0: no such member. -1: a member that is no DateTime (reported by the caller).
// The member may be an ISO 8601 string, a JSON-LD value object ({"@value": "..."}), or an
// integer (epoch nanoseconds).
//
extern int64_t migrateTimeTake(CorNode* objP, CorTerm term, const char* name);



// -----------------------------------------------------------------------------
//
// migrateTimeOf - a DateTime node as epoch ns (0: not a DateTime)
//
extern int64_t migrateTimeOf(CorNode* nodeP);



// -----------------------------------------------------------------------------
//
// migrateStringTake - take a string member out of an object (NULL: none)
//
extern const char* migrateStringTake(CorNode* objP, const char* name);



// -----------------------------------------------------------------------------
//
// migrateProblem - the detail of the problem the broker's code reported, and reset it
//
extern const char* migrateProblem(void);

#endif  // MIGRATE_MIGRATEUTIL_H_
