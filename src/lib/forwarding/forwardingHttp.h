#ifndef FORWARDING_FORWARDINGHTTP_H_
#define FORWARDING_FORWARDINGHTTP_H_

//
// FILE            forwardingHttp.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// HTTP / HTTPS forwarding plugin — registers itself for the "http" and
// "https" URL schemes against the LdForwarding registry. Built into
// coraine (HTTP is the always-available default transport).
//

// -----------------------------------------------------------------------------
//
// forwardingHttpRegister - register the HTTP plugin at startup
//
// Called once from coraine.c during init, before any service routine
// runs. Idempotent (re-registration of an already-claimed scheme is a
// no-op).
//
extern void forwardingHttpRegister(void);

#endif  // FORWARDING_FORWARDINGHTTP_H_
