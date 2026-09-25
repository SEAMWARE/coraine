#ifndef CORAINE_PATCH_ENTITY_ATTRS_H_
#define CORAINE_PATCH_ENTITY_ATTRS_H_

//
// FILE            patchEntityAttrs.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// PATCH /ngsi-ld/v1/entities/{entityId}/attrs — Update Attributes
// (NGSI-LD § 5.6.2 / § 6.6.3.2).
//

#include <stdbool.h>                              // bool
#include <stdint.h>                               // uint64_t

#include "kjson/KjNode.h"                         // KjNode

extern bool patchEntityAttrs(void);



// -----------------------------------------------------------------------------
//
// patchEntityAttrsOn - Update Attributes of entityId with fragment, as the route does
//
// The route, with its input given instead of read from the request: what a goal
// POSTed to a Channel does to its entity is exactly what a PATCH of the
// attribute does - the transport deciding first, the notifications, TRoE.
// goalIdP: NULL, or where to put the transport's id of the goal the write sent
// and that was accepted (NULL: none, or the transport gave no id).
//
// fragment must be what the route gets: EXPANDED, by corLdExpandTree - the
// nodes carry the classification bits the NGSI-LD layer reads.
//
extern bool patchEntityAttrsOn(const char* entityId, KjNode* fragment, char** goalIdP);

#endif  // CORAINE_PATCH_ENTITY_ATTRS_H_
