//
// FILE            seCoreTerms.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                   // NULL

#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corLog/corLog.h"                            // COR_T
#include "corJsonld/corLdInit.h"                      // CorLdCoreTerm, corLdCoreTermsAdd

#include "serviceExecution/seCoreTerms.h"             // Own interface
#include "coraineTraceLevels.h"                       // CtService



// -----------------------------------------------------------------------------
//
// seCoreTermV - every term the payloads use that the published core context lacks
//
// Reused from the core as they are: id, type, title, description, mode, endpoint, entities, entityId,
// q, geoQ, status, notification.
//
static const CorLdCoreTerm seCoreTermV[] =
{
  { "ServiceDescription",          NULL     },
  { "ServiceRegistration",         NULL     },
  { "ServiceExecution",            NULL     },
  { "GroupedServiceExecution",     NULL     },
  { "CombinedServiceExecution",    NULL     },
  { "CombinedServiceTemplate",     NULL     },
  { "AttributeSelector",           NULL     },
  { "attributeSelector",           NULL     },   // a q-language attribute path - resolved when the execution runs
  { "serviceName",                 "@vocab" },   // a service is named like an attribute, and expanded as one
  { "serviceInformation",          NULL     },
  { "serviceDescriptionInEntity",  NULL     },
  { "executionStatus",             NULL     },
  { "combinationMethod",           NULL     },
  { "serviceExecutions",           NULL     },
  { "services",                    NULL     },
  { "templateName",                NULL     },
  { "entityType",                  "@vocab" },   // the value is an entity type - expanded as one
  { "inputSchema",                 "@json"  },   // a JSON Schema - verbatim
  { "outputSchema",                "@json"  },   // a JSON Schema - verbatim
  { "executionInput",              "@json"  },   // the executor's values - verbatim
  { "executionOutput",             "@json"  },   // the executor's result - verbatim
  { "executionError",              "@json"  },   // a ProblemDetails - verbatim
  { "executionProgress",           "@json"  },   // the executor's progress - verbatim
  { "executionStartedAt",          NULL     },
  { "executionEndedAt",            NULL     },
  { "executionTimeout",            NULL     },   // an ISO 8601 duration
  { "serviceTemplateId",           "@id"    },   // a Combined Service Template's id
  { NULL,                          NULL     }
};



// -----------------------------------------------------------------------------
//
// seCoreTermsAdd -
//
int seCoreTermsAdd(CorAlloc* kaP)
{
  int added = corLdCoreTermsAdd(seCoreTermV, kaP);

  if (added < 0)
    return -1;

  COR_T(CtService, "%d Service Execution terms added to the core context", added);
  return 0;
}
