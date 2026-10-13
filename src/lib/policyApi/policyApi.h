//
// FILE            policyApi.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#ifndef SRC_LIB_POLICYAPI_POLICYAPI_H_
#define SRC_LIB_POLICYAPI_POLICYAPI_H_

#include <stdbool.h>                                  // bool



// -----------------------------------------------------------------------------
//
// policyApiStart - the ODRL-PAP's /policy API (odrl-pap 1.7.1, api/odrl.yaml) on a listener of its own
//
// --policyApiPort / --policyApiHost: the policies in the default tenant's document collection
// "odrlPolicies", served to whoever reaches the port - as the PAP is, with no authentication; the auth
// plugin is not asked. fdsc.so reads them with --fdscPap http://<host>:<port>, as it reads the PAP's.
// After corRestInit. false: not listening (the reason logged).
//
extern bool policyApiStart(const char* host, unsigned short port);

#endif  // SRC_LIB_POLICYAPI_POLICYAPI_H_
