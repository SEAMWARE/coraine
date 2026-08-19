#ifndef SERVICE_ROUTINES_REPLACE_ENTITY_H_
#define SERVICE_ROUTINES_REPLACE_ENTITY_H_

//
// FILE            replaceEntity.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                                   // bool



// -----------------------------------------------------------------------------
//
// replaceEntity - PUT /ngsi-ld/v1/entities/{entityId}
//
// NGSI-LD v1.9.1 §5.6.16 (Replace Entity) / §5.5.12. Replaces a stored
// entity with the payload. Id and type must not change.
//
extern bool replaceEntity(void);

#endif  // SERVICE_ROUTINES_REPLACE_ENTITY_H_
