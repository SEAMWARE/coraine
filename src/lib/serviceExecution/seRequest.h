#ifndef SRC_LIB_SERVICEEXECUTION_SEREQUEST_H_
#define SRC_LIB_SERVICEEXECUTION_SEREQUEST_H_

//
// FILE            seRequest.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The request's own parts that Service Execution reads - a header, a URL parameter.
//
// SE_PARAM_* - the URL parameters of GET /ngsi-ld/v1/services, registered by the broker (corRestParamAdd).
// LD_PARAM_* grow from bit 0, the broker's own from the top (bridgeServiceSync.h: ddsSync, bit 63).
//
#define SE_PARAM_ENTITY_ID         (1ULL << 62)
#define SE_PARAM_SERVICE_NAME      (1ULL << 61)
#define SE_PARAM_EXECUTION_STATUS  (1ULL << 60)



// -----------------------------------------------------------------------------
//
// seRequestHeader - a request header's value, NULL if absent (case-insensitive)
//
extern const char* seRequestHeader(const char* key);



// -----------------------------------------------------------------------------
//
// seRequestParam - a URL parameter's value, NULL if absent
//
extern const char* seRequestParam(const char* key);

#endif  // SRC_LIB_SERVICEEXECUTION_SEREQUEST_H_
