#ifndef CORAINE_TRACE_LEVELS_H_
#define CORAINE_TRACE_LEVELS_H_

//
// FILE            coraineTraceLevels.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
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
  KtDistOpRequest = 400,  // Outbound distributed-operation request URL
  KtHa            = 401   // Cache sync with the other broker instances
};

#endif  // CORAINE_TRACE_LEVELS_H_
