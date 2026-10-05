#ifndef CORAINE_TRACE_LEVELS_H_
#define CORAINE_TRACE_LEVELS_H_

//
// FILE            coraineTraceLevels.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Trace levels for coraine (range 400-599).
//
// Reserved by other libraries:
//   corRest    100-199   (Cort*)
//   corNgsild  200-399   (LdT*)
//   coraine  400-599   (Kt*)
//
enum CorBrokerTraceLevel
{
  CtDistOpRequest = 400,  // Outbound distributed-operation request URL
  CtHa            = 401,  // Cache sync with the other broker instances
  CtBridge        = 402,  // Bridge plugins: transports to non-NGSI-LD peers
  CtService       = 403   // Service Execution: registrations, executions, executors
};

#endif  // CORAINE_TRACE_LEVELS_H_
