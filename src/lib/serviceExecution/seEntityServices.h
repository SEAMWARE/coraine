#ifndef SRC_LIB_SERVICEEXECUTION_SEENTITYSERVICES_H_
#define SRC_LIB_SERVICEEXECUTION_SEENTITYSERVICES_H_

//
// FILE            seEntityServices.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                          // CorNode



// -----------------------------------------------------------------------------
//
// seEntityServicesAdd - ?includeServices=true: each service an entity has, as an attribute (GR CIM-055 § 6.4.2/3)
//
// For every Service Registration that applies to the entity: an attribute named by its serviceName,
//
//   { "type": "ServiceDescription", "title"?, "description"?, "mode", "serviceDescriptionInEntity": false }
//
// and, with ?serviceDetails=true, its inputSchema and outputSchema. Never the executor's endpoint.
// 'treeP' is an entity, or an array of them (a query); without ?includeServices=true nothing happens.
//
extern void seEntityServicesAdd(CorNode* treeP);

#endif  // SRC_LIB_SERVICEEXECUTION_SEENTITYSERVICES_H_
