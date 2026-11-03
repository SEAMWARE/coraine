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
// id and type are mandatory Entity members (§ 5.2.4), never Attributes: no Attribute by that name can be
// modified, replaced or deleted through .../attrs/{attrId}, and the request is refused outright - the
// operations' "the target Attribute name is not a valid name" (BadRequestData). Let through, PATCH and PUT
// took the fragment for the Entity's new id or type, and refused it only because an object is neither (a
// 400 for the wrong reason), and PUT .../value looked for an Attribute and answered 404 - which reads as
// "this Entity happens not to carry it", and invites a retry.
//
bool attrPathEntityMember(const char* attrWild, const char* attrIri, const char* operation)
{
  bool member = (strcmp(attrWild, "id")   == 0) || (strcmp(attrWild, "@id")   == 0) ||
                (strcmp(attrWild, "type") == 0) || (strcmp(attrWild, "@type") == 0) ||
                ((attrIri != NULL) && ((strcmp(attrIri, "@id") == 0) || (strcmp(attrIri, "@type") == 0)));

  if (member == false)
    return false;

  ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Attribute name",
          "'%s' is a mandatory Entity member, not an Attribute - it cannot be %s", attrWild, operation);
  return true;
}
