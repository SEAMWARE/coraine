//
// FILE            attrPathEntityMember.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                 // bool
#include <string.h>                                  // strcmp

#include "corNgsild/corNgsild.h"                     // ldError, LD_ERROR_BAD_REQUEST_DATA

#include "serviceRoutines/attrPathEntityMember.h"    // Own interface



// -----------------------------------------------------------------------------
//
// attrPathEntityMember -
//
// id, type and scope are Entity members (§ 5.2.4), not Attributes: .../attrs/{attrId} names none of them,
// and Partial Attribute Update, Replace Attribute, Set Attribute Value and Delete Attribute refuse it -
// BadRequestData. A term of the request's @context expanding to @id, @type or ngsi-ld:scope is the same.
//
// scope is changed through the Entity (Merge Entity, Update Attributes, Replace Entity), never through
// .../attrs/scope - Delete Attribute included, where § 10.2.7.4 has "If the target Attribute is scope,
// remove the scope" - coraine refuses it too.
//
bool attrPathEntityMember(const char* attrWild, const char* attrIri, const char* operation)
{
  bool member = (strcmp(attrWild, "id")   == 0) || (strcmp(attrWild, "@id")   == 0) ||
                (strcmp(attrWild, "type") == 0) || (strcmp(attrWild, "@type") == 0) ||
                (strcmp(attrWild, "scope") == 0) ||
                ((attrIri != NULL) && ((strcmp(attrIri, "@id")   == 0) ||
                                       (strcmp(attrIri, "@type") == 0) ||
                                       (strcmp(attrIri, "https://uri.etsi.org/ngsi-ld/scope") == 0)));

  if (member == false)
    return false;

  ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Attribute name",
          "'%s' is an Entity member, not an Attribute - it cannot be %s through .../attrs/%s", attrWild, operation, attrWild);
  return true;
}
