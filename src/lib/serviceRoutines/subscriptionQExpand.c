//
// FILE            subscriptionQExpand.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                  // NULL

#include "corRest/CorRestState.h"                    // corRest
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeLookup.h"                   // corTreeLookup

#include "corNgsild/CorNgsild.h"                     // corNgsild
#include "corNgsild/LdQ.h"                           // LdQNode
#include "corNgsild/ldQParse.h"                      // ldQParse
#include "corNgsild/ldQRender.h"                     // ldQRenderStored

#include "corNgsild/ldQExpandValues.h"                   // ldQExpandValues, ldQLangProperties, ldAttrListExpand
#include "serviceRoutines/subscriptionQExpand.h"     // Own interface



// -----------------------------------------------------------------------------
//
// subscriptionQExpand -
//
// An attribute's name is its long name; the short name in a q is only an alias, meaningful in
// the @context it traveled with - the request's. The stored q outlives that request: the
// subscription caches parse it again at every restart, and the HA watch on another broker, with
// no context at all. So it is stored with its attribute names EXPANDED - ldQRenderStored, where
// an IRI's dots are written '^' so they do not read as the sub-attribute separator - and ldQParse
// reads that back with no context. Stored as sent, a q naming terms of the client's @context was
// parsed after a restart with the core context alone, and matched nothing it was meant to.
//
void subscriptionQExpand(CorNode* subP)
{
  CorNode* qP = corTreeLookup(subP, "q");

  if ((qP == NULL) || (qP->type != CorString))
    return;

  LdQNode* qExprP = ldQParse(qP->value.s, &corRest.kalloc);   // expands the names via corNgsild.contextP

  if (qExprP == NULL)
    return;

  //
  // And what goes WITH q is applied before it is stored, with the context it traveled with - the
  // only one that gives its terms their meaning: the values expandValues names are stored expanded,
  // and a [..] under an attribute langProperties names is stored as the language tag it is (not
  // NGSI-LD, spec-doubts #134). The stored q is then fully resolved, and ldQParseStored reads it back
  // with no context at all.
  //
  CorNode* evP = corTreeLookup(subP, "expandValues");
  CorNode* lpP = corTreeLookup(subP, "langProperties");
  char**   evV = ((evP != NULL) && (evP->type == CorString)) ? ldAttrListExpand(evP->value.s, corNgsild.contextP, &corRest.kalloc) : NULL;
  char**   lpV = ((lpP != NULL) && (lpP->type == CorString)) ? ldAttrListExpand(lpP->value.s, corNgsild.contextP, &corRest.kalloc) : NULL;

  ldQExpandValues(qExprP, evV, corNgsild.contextP, &corRest.kalloc);

  ldQLangProperties(qExprP, lpV);

  char* storedQ = ldQRenderStored(qExprP, &corRest.kalloc);

  if (storedQ != NULL)
    qP->value.s = storedQ;
}
