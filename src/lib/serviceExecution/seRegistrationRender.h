#ifndef SRC_LIB_SERVICEEXECUTION_SEREGISTRATIONRENDER_H_
#define SRC_LIB_SERVICEEXECUTION_SEREGISTRATIONRENDER_H_

//
// FILE            seRegistrationRender.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corTree/CorNode.h"                          // CorNode



// -----------------------------------------------------------------------------
//
// seRegistrationRender - a stored Service Registration made ready for a response
//
// createdAt / modifiedAt as ISO 8601 with sysAttrs, else gone; q compacted with the request's
// @context (it is stored expanded). The rest - entities[].type, serviceName - the render hook
// compacts, as every response. Call ldContextResolve first.
//
extern void seRegistrationRender(CorNode* regP);

#endif  // SRC_LIB_SERVICEEXECUTION_SEREGISTRATIONRENDER_H_
