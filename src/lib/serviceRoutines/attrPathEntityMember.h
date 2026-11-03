//
// FILE            attrPathEntityMember.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#ifndef SERVICEROUTINES_ATTRPATHENTITYMEMBER_H_
#define SERVICEROUTINES_ATTRPATHENTITYMEMBER_H_

#include <stdbool.h>                                 // bool



// -----------------------------------------------------------------------------
//
// attrPathEntityMember - the {attrId} of a .../attrs/{attrId} route names an Entity member (id, type), not an
// Attribute: the request is refused (400 BadRequestData, the answer rendered) and true returned
//
// attrWild is the name as the URL path has it, attrIri as the request's @context expands it (an alias of
// @id or @type in a user @context is caught there). 'operation' says what was refused - "modified",
// "replaced", "deleted" - for the error's detail.
//
extern bool attrPathEntityMember(const char* attrWild, const char* attrIri, const char* operation);

#endif  // SERVICEROUTINES_ATTRPATHENTITYMEMBER_H_
