#ifndef SRC_LIB_SERVICEEXECUTION_SECOMBINED_H_
#define SRC_LIB_SERVICEEXECUTION_SECOMBINED_H_

//
// FILE            seCombined.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Combined and Grouped Service Executions, and Combined Service Templates (GR CIM-055 § 6.3.5.2-4,
// § 6.4.7.3-5, § 6.4.14-17).
//
// A combined or grouped execution is a document of the "serviceExecutions" collection like a simple one,
// with its children's ids in "_children"; each child names it in "_parent". Its combinationMethod:
//
//   parallel     every child started at once; it ends when they all have - completed when they all
//                completed, failed otherwise
//   sequential   the next child started when one completed; a child that failed (or was cancelled)
//                ends it - failed, the children not started yet cancelled
//
// conditional, timetriggered and recurring are named by the report, and "to be specified" there:
// refused, 501.
//
// A child is a simple execution, or a combined one (nesting). A grouped execution's children are one
// simple execution per entity that matches its selection (entityType, idPattern, q, geoQ) and offers the
// service.
//
#include <stdbool.h>                                  // bool

#include "corTree/CorNode.h"                          // CorNode



// -----------------------------------------------------------------------------
//
// seCombinedCreate - POST /services with a CombinedServiceExecution or a GroupedServiceExecution
//
// Every child is built - its entity, its service, its input - before anything is stored: a refusal
// leaves nothing behind. Then the execution is started, and answered 201 (Location, Service-Execution).
//
extern bool seCombinedCreate(CorNode* bodyP);



// -----------------------------------------------------------------------------
//
// seCombinedChildEnded - a child ended: the parent's next child started, or the parent ended
//
extern void seCombinedChildEnded(const char* parentId);



// -----------------------------------------------------------------------------
//
// seCombinedCancel - DELETE of a combined or grouped execution: running, its running children
// cancelled (409 if one cannot be), the rest cancelled; ended, it and its children deleted
//
extern bool seCombinedCancel(CorNode* execP);



// -----------------------------------------------------------------------------
//
// seCombinedChildrenEmbed - its children, as "serviceExecutions" (rendered), in place of "_children"
//
extern void seCombinedChildrenEmbed(CorNode* execP);



// -----------------------------------------------------------------------------
//
// seTemplateCheck - is this a Combined Service Template? false: a 400 raised
//
extern bool seTemplateCheck(CorNode* templateP);

#endif  // SRC_LIB_SERVICEEXECUTION_SECOMBINED_H_
