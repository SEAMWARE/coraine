#ifndef SRC_LIB_SERVICEEXECUTION_SEREGISTRATIONMATCH_H_
#define SRC_LIB_SERVICEEXECUTION_SEREGISTRATIONMATCH_H_

//
// FILE            seRegistrationMatch.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                  // bool

#include "corTree/CorNode.h"                          // CorNode



// -----------------------------------------------------------------------------
//
// seRegistrationMatches - does a stored Service Registration apply to this entity?
//
// regP       the registration as stored (entities[].type expanded, q stored expanded)
// entityId   NULL: any id - an idPattern or id of the registration is then no obstacle
// typeV      the entity's types, expanded, NULL-terminated; NULL: any type
// entityP    the entity as stored, or NULL: the registration's q, scopeQ and geoQ are not evaluated
//
// One member of 'entities' matching (type, then id or idPattern) is enough; then the registration's
// q, scopeQ (on the entity's scope) and geoQ must hold for the entity.
//
extern bool seRegistrationMatches(CorNode* regP, const char* entityId, char** typeV, CorNode* entityP);



// -----------------------------------------------------------------------------
//
// seRegistrationNameConflict - a service name is unique within an entity
//
// regP       a registration about to be created or replaced (as it will be stored)
//
// Returns the id of another registration of the tenant with the same serviceName whose 'entities' could
// select an entity this one's also could (of several, the first by id); NULL: none.
//
// Conservative: only what proves two selectors disjoint separates them - different types, different ids,
// an id the other's idPattern does not match, two anchored idPatterns whose literal prefixes neither
// starts the other. q, geoQ and scopeQ do not separate (two filters cannot be proven disjoint).
//
extern const char* seRegistrationNameConflict(CorNode* regP);

#endif  // SRC_LIB_SERVICEEXECUTION_SEREGISTRATIONMATCH_H_
