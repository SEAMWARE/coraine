#ifndef SERVICE_ROUTINES_DELETE_JSONLD_CONTEXT_H_
#define SERVICE_ROUTINES_DELETE_JSONLD_CONTEXT_H_

//
// FILE            deleteJsonldContext.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                                   // bool



// -----------------------------------------------------------------------------
//
// deleteJsonldContext - DELETE /ngsi-ld/v1/jsonldContexts/{contextId}
//
// NGSI-LD v1.9.1 § 5.13.3 (Delete and Reload JSON-LD Context).
//
extern bool deleteJsonldContext(void);

#endif
