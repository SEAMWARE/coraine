#ifndef CURRENTSTATE_MONGOC_MONGOCPING_H_
#define CURRENTSTATE_MONGOC_MONGOCPING_H_

//
// FILE            mongocPing.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//



// -----------------------------------------------------------------------------
//
// mongocPing - DbDriver.ping: a 'ping' command on a client of its own, bounded by timeoutMs
//
extern int mongocPing(int timeoutMs);



// -----------------------------------------------------------------------------
//
// mongocPingClose - destroy the ping's client (mongocClose, once nothing pings any more)
//
extern void mongocPingClose(void);

#endif  // CURRENTSTATE_MONGOC_MONGOCPING_H_
