//
// FILE            subscriptionQExpand.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#ifndef SERVICEROUTINES_SUBSCRIPTIONQEXPAND_H_
#define SERVICEROUTINES_SUBSCRIPTIONQEXPAND_H_

#include "corTree/CorNode.h"                         // CorNode



// -----------------------------------------------------------------------------
//
// subscriptionQExpand - replace a subscription's q with its STORED form: attribute names expanded
//
// With the request's @context (corNgsild.contextP) - see subscriptionQExpand.c.
//
extern void subscriptionQExpand(CorNode* subP);

#endif  // SERVICEROUTINES_SUBSCRIPTIONQEXPAND_H_
