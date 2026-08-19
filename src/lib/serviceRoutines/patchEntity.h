#ifndef PATCH_ENTITY_H
#define PATCH_ENTITY_H

//
// FILE            patchEntity.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                              // bool



// -----------------------------------------------------------------------------
//
// patchEntity - PATCH /ngsi-ld/v1/entities/{entityId}
//
// Merge Entity operation per ETSI GS CIM 009 v1.9.1 § 5.6.17. Applies RFC 7396
// merge semantics (with "urn:ngsi-ld:null" as the delete-marker) to an
// existing entity.
//
extern bool patchEntity(void);

#endif  // PATCH_ENTITY_H
