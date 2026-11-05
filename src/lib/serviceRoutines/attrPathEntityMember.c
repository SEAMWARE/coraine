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
// and Partial Attribute Update, Replace Attribute and Set Attribute Value refuse it - BadRequestData
// (§ 10.2.5.4, § 10.2.6.4, § 10.2.11.4 for scope). A term of the request's @context expanding to @id, @type
// or ngsi-ld:scope is the same.
//
// Delete Attribute refuses id and type only: § 10.2.7.4 has "If the target Attribute is scope, remove the
// scope Attribute from the target Entity" - deleteEntityAttr takes scope before calling this.
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
