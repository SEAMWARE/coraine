#ifndef SERVICE_ROUTINES_GET_JSONLD_CONTEXT_H_
#define SERVICE_ROUTINES_GET_JSONLD_CONTEXT_H_

//
// FILE            getJsonldContext.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                                   // bool



// -----------------------------------------------------------------------------
//
// getJsonldContext - GET /ngsi-ld/v1/jsonldContexts/{contextId}
//
// NGSI-LD v1.9.1 § 5.13.4 (Retrieve JSON-LD Context).
//
extern bool getJsonldContext(void);

#endif  // SERVICE_ROUTINES_GET_JSONLD_CONTEXT_H_
