//
// FILE            startupCoreContext.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                       // NULL

#include "corAlloc/CorAlloc.h"                            // CorAlloc
#include "corJsonld/corJsonld.h"                          // corLdInit

#include "corNgsild/corNgsild.h"                          // ldInit
#include "corNgsild/ldCoreTermIds.h"                      // ldCoreTermIdsInit
#include "corNgsild/ldExtensionTerms.h"                   // ldExtensionTermsAdd
#if COR_FEATURE_SERVICE_EXECUTION
#include "corNgsild/ldServiceDescription.h"               // ldServiceDescriptionAccepted
#include "serviceExecution/seCoreTerms.h"                 // seCoreTermsAdd
#endif

#include "bridge/bridgeCoreTerms.h"                       // bridgeCoreTermsAdd
#include "startup/startupContext.h"                       // startupContextDownload, startupContextError
#include "startup/startupCoreContext.h"                   // Own interface



// -----------------------------------------------------------------------------
//
// startupCoreContext -
//
const char* startupCoreContext(CorAlloc* contextAllocP)
{
  if (corLdInit(contextAllocP, NULL, startupContextDownload, startupContextError) != 0)
    return "corLdInit failed";

  //
  // The ContextBridge / Channel / Goal terms are core terms - with or without
  // --bridges: whether a term expands must not depend on a startup flag.
  //
  if (bridgeCoreTermsAdd(contextAllocP) != 0)
    return "the ContextBridge/Channel terms could not be added to the core context";

  //
  // ... and so are the terms of coraine's NGSI-LD extensions (langProperties - spec-doubts #134):
  // short names in a Query body, and in a Subscription that is stored.
  //
  if (ldExtensionTermsAdd(contextAllocP) != 0)
    return "the NGSI-LD extension terms could not be added to the core context";

#if COR_FEATURE_SERVICE_EXECUTION
  //
  // ... and Service Execution's (doc/service-execution.md) - to enter the spec, core terms here already
  //
  if (seCoreTermsAdd(contextAllocP) != 0)
    return "the Service Execution terms could not be added to the core context";

  ldServiceDescriptionAccepted = true;               // an entity may hold its services' descriptions (GR CIM-055 § 6.3.3)
#endif

  //
  // Every core term gets its CorTerm id - after the Bridge/Channel terms, which are core terms too.
  //
  if (ldCoreTermIdsInit(contextAllocP) != 0)
    return "the core context terms could not be given their CorTerm ids";

  if (ldInit() != 0)
    return "ldInit failed";

  return NULL;
}
