#ifndef SRC_APP_CORAINE_HEALTH_H_
#define SRC_APP_CORAINE_HEALTH_H_

//
// FILE            health.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                   // bool



// -----------------------------------------------------------------------------
//
// The health port (--healthPort) - liveness and readiness for an orchestrator, outside the HTTP server
//
// A port of its own, served by a thread of its own: a probe never waits in the HTTP server's queue
// behind the requests, so a broker that is busy is not mistaken for one that is dead. One connection at
// a time - an answer takes microseconds: the thread does no I/O for it, it formats what background
// samplers keep current:
//
//   requests   in flight (and since when), finished, when the last one finished - a slot a request,
//              taken in the pre-dispatch hook and freed in the post-response hook
//   store      the current-state DB: a ping a second on a connection of its own (DbDriver.ping),
//              bounded by a timeout - NULL ping: in the broker's process (corDB), reachable once loaded
//   troe       the same, for the temporal store (TroeDriver.ping)
//   memory     the memory budget's figures (memoryBudget.h), sampled once a second
//
//   GET /live    200 - 503 only when a request has been in flight for longer than --healthStallTimeout
//                seconds and none has finished in that time: a deadlock. Never for memory or a
//                database: a restart cures neither
//   GET /ready   503 while the store loads at startup, while the store or the temporal store does not
//                answer its ping, over the hard memory budget, when stalled, and once the broker is
//                stopping; else 200
//   GET <other>  200
//
// Every answer carries the same JSON report. A connection that sends nothing (a TCP probe: connect,
// close) is closed unanswered.
//
// Opened before the store loads, so /live answers through a long load of a persistent corDB and /ready
// turns 200 only once it is done.
//



// -----------------------------------------------------------------------------
//
// healthStart - open the port and start its thread; false: the port could not be opened
//
// After ldInit (the request tracking chains on corNgsild's pre-dispatch hook) and before dbStart.
//
extern bool healthStart(unsigned short port, int stallSecs);



// -----------------------------------------------------------------------------
//
// healthStoreLoaded / healthTroeLoaded - dbStart / troeStart returned: the store's pinger starts
//
extern void healthStoreLoaded(void);
extern void healthTroeLoaded(void);



// -----------------------------------------------------------------------------
//
// healthServing - the HTTP server takes requests: /ready may answer 200
//
extern void healthServing(void);



// -----------------------------------------------------------------------------
//
// healthRequestEnd - a request has finished (the post-response hook)
//
extern void healthRequestEnd(void);



// -----------------------------------------------------------------------------
//
// healthStopping - SIGTERM: /ready answers 503 from here on, the pingers stop after their current ping
//
extern void healthStopping(void);



// -----------------------------------------------------------------------------
//
// healthStop - wait for the pingers to have stopped - before dbClose, which frees what they ping with
//
// The port itself stays open until the process exits: /live keeps answering through the shutdown
// (a persistent corDB writes its final snapshot in dbClose).
//
extern void healthStop(void);

#endif  // SRC_APP_CORAINE_HEALTH_H_
