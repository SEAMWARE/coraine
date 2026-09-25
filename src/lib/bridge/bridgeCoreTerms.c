//
// FILE            bridgeCoreTerms.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                   // NULL

#include "kalloc/KAlloc.h"                            // KAlloc
#include "ktrace/kTrace.h"                            // KT_T
#include "corJsonld/corLdInit.h"                      // CorLdCoreTerm, corLdCoreTermsAdd

#include "bridge/bridgeCoreTerms.h"                   // Own interface
#include "coraineTraceLevels.h"                       // KtBridge



// -----------------------------------------------------------------------------
//
// bridgeCoreTermV - every term the payloads use that the published core context lacks
//
// Reused from the core as it is: endpoint, status, entityId, entity.
//
static const CorLdCoreTerm bridgeCoreTermV[] =
{
  { "ContextBridge",     NULL     },   // ⚠ provisional - "Bridge" is schema.org's
  { "Channel",           NULL     },
  { "Goal",              NULL     },
  { "bridgeId",          "@id"    },
  { "bridgeOptions",     "@json"  },   // its keys are the transport's vocabulary - verbatim, as core "json" is
  { "plugin",            NULL     },
  { "channelTarget",     NULL     },
  { "channelKind",       NULL     },
  { "channelDirection",  NULL     },
  { "retention",         NULL     },
  { "codec",             NULL     },
  { "entityAttribute",   "@vocab" },   // the value is an Attribute name - expanded as core "attributes" is
  { "statusReason",      NULL     },
  { "goals",             NULL     },
  { "goalId",            NULL     },
  { "goalRequest",       "@json"  },   // the transport's own JSON - verbatim
  { "goalFeedback",      "@json"  },   // the transport's own JSON - verbatim
  { "goalResult",        "@json"  },   // the transport's own JSON - verbatim
  { NULL,                NULL     }
};



// -----------------------------------------------------------------------------
//
// bridgeCoreTermsAdd -
//
int bridgeCoreTermsAdd(KAlloc* kaP)
{
  int added = corLdCoreTermsAdd(bridgeCoreTermV, kaP);

  if (added < 0)
    return -1;

  KT_T(KtBridge, "%d ContextBridge/Channel/Goal terms added to the core context", added);
  return 0;
}
