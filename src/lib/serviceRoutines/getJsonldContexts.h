#ifndef SERVICE_ROUTINES_GET_JSONLD_CONTEXTS_H_
#define SERVICE_ROUTINES_GET_JSONLD_CONTEXTS_H_

//
// FILE            getJsonldContexts.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stdbool.h>                                   // bool



// -----------------------------------------------------------------------------
//
// getJsonldContexts - GET /ngsi-ld/v1/jsonldContexts
//
// NGSI-LD v1.9.1 § 5.13.5 (Retrieve Available JSON-LD Contexts).
//
extern bool getJsonldContexts(void);

#endif  // SERVICE_ROUTINES_GET_JSONLD_CONTEXTS_H_
