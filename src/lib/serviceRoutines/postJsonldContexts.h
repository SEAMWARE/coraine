#ifndef SERVICE_ROUTINES_POST_JSONLD_CONTEXTS_H_
#define SERVICE_ROUTINES_POST_JSONLD_CONTEXTS_H_

//
// FILE            postJsonldContexts.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                                   // bool



// -----------------------------------------------------------------------------
//
// postJsonldContexts - POST /ngsi-ld/v1/jsonldContexts
//
// NGSI-LD v1.9.1 § 5.13.2 (Add JSON-LD Context).
//
extern bool postJsonldContexts(void);

#endif
