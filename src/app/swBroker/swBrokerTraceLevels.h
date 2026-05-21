#ifndef SWBROKER_SW_BROKER_TRACE_LEVELS_H_
#define SWBROKER_SW_BROKER_TRACE_LEVELS_H_

//
// FILE            swBrokerTraceLevels.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Trace levels for swBroker (range 400-599).
//
// Reserved by other libraries:
//   swRest    100-199   (Swt*)
//   swNgsild  200-399   (LdT*)
//   swBroker  400-599   (Kt*)
//
enum SwBrokerTraceLevel
{
  KtDistOpRequest = 400   // Outbound distributed-operation request URL
};

#endif  // SWBROKER_SW_BROKER_TRACE_LEVELS_H_
