#ifndef POST_ENTITY_BATCH_CREATE_H
#define POST_ENTITY_BATCH_CREATE_H

//
// FILE            postEntityBatchCreate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                              // bool



// -----------------------------------------------------------------------------
//
// postEntityBatchCreate - POST /ngsi-ld/v1/entityOperations/create (§ 5.6.7)
//
extern bool postEntityBatchCreate(void);

#endif  // POST_ENTITY_BATCH_CREATE_H
