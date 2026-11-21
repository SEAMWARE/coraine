//
// FILE            entityTypeSelected.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#ifndef SERVICEROUTINES_ENTITYTYPESELECTED_H_
#define SERVICEROUTINES_ENTITYTYPESELECTED_H_

#include <stdbool.h>                                 // bool

#include "corTree/CorNode.h"                         // CorNode



// -----------------------------------------------------------------------------
//
// entityTypeSelected - the locally held Entity is selected by the request's ?type (a selector of Entity types,
// TS 104-175 clause 7.2.2): true without ?type, or when one of the Entity's types matches it
//
// entityP is the Entity as the store returns it (expanded types). A write routine that finds the Entity NOT
// selected treats it as not held locally - the same path as DB_NOT_FOUND, so a forwarded part still counts.
//
extern bool entityTypeSelected(CorNode* entityP);

#endif  // SERVICEROUTINES_ENTITYTYPESELECTED_H_
