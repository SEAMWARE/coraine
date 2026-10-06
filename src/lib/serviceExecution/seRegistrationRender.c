//
// FILE            seRegistrationRender.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corRest/CorRestState.h"                     // corRest
#include "corNgsild/CorNgsild.h"                      // corNgsild
#include "corNgsild/ldQParse.h"                       // ldQParseStored
#include "corNgsild/ldSubscriptionCompactQ.h"         // ldSubscriptionCompactQ
#include "corNgsild/ldStripSysAttrs.h"                // ldStripSysAttrs
#include "corNgsild/ldSysTimestamp.h"                 // ldSysTimestampsToIso

#include "serviceExecution/seRegistrationRender.h"    // Own interface



// -----------------------------------------------------------------------------
//
// seRegistrationRender -
//
void seRegistrationRender(CorNode* regP)
{
  if (corNgsild.sysAttrs == false)
    ldStripSysAttrs(regP);
  else
    ldSysTimestampsToIso(regP, &corRest.kalloc);

  CorNode* qP = corTreeLookup(regP, "q");

  if ((qP != NULL) && (qP->type == CorString))
    ldSubscriptionCompactQ(regP, ldQParseStored(qP->value.s, &corRest.kalloc), corNgsild.contextP, &corRest.kalloc);
}
