//
// FILE            health.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                   // bool
#include <stdint.h>                                    // int64_t, uint64_t
#include <stdio.h>                                     // snprintf
#include <string.h>                                    // strchr, strstr, strcmp, strncmp, strcspn
#include <unistd.h>                                    // close
#include <errno.h>                                     // errno, EINTR
#include <time.h>                                      // clock_gettime, nanosleep
#include <poll.h>                                      // poll
#include <pthread.h>                                   // pthread_create, pthread_join
#include <stdatomic.h>                                 // _Atomic, atomic_*
#include <sys/socket.h>                                // socket, bind, listen, accept, recv, send, shutdown
#include <sys/time.h>                                  // struct timeval
#include <netinet/in.h>                                // sockaddr_in, sockaddr_in6, in6addr_any

#include "corLog/corLog.h"                             // COR_I, COR_W
#include "corRest/CorRestState.h"                      // corRestP
#include "corRest/corRestHooks.h"                      // CorRestHook, corRestSetPreDispatchHook, corRestPreDispatchHookGet

#include "db/DbDriver.h"                               // db
#include "troe/TroeDriver.h"                           // troe

#include "memoryBudget.h"                              // memoryBudgetValues, memoryBudgetLevel
#include "health.h"                                    // Own interface



// -----------------------------------------------------------------------------
//
// Pinger - a store's ping, a thread of its own: one that hangs keeps the other's answers current
//
typedef struct Pinger
{
  const char*      what;                               // for the log: "the store", "the temporal store"
  int            (*ping)(int timeoutMs);               // DbDriver.ping / TroeDriver.ping - 0: answered
  pthread_t        tid;
  bool             running;
  _Atomic bool     ok;                                 // the last ping was answered
  _Atomic int64_t  lastMs;                             // monotonic ms the last ping ended - 0: none yet
  _Atomic int64_t  latencyUs;                          // how long the last ping took
} Pinger;



// -----------------------------------------------------------------------------
//
// State
//
static bool              enabled        = false;      // --healthPort given: set once, before any request
static int               stallMs        = 30000;
static int64_t           startMs        = 0;
static int               listenFd       = -1;
static CorRestHook       preDispatchNext = NULL;
static _Atomic bool      stopping       = false;
static _Atomic bool      storeLoaded    = false;
static _Atomic bool      troeLoaded     = false;
static _Atomic bool      serving        = false;
static _Atomic int64_t   finished       = 0;
static _Atomic int64_t   lastFinishedMs = 0;          // monotonic - 0: none yet
static Pinger            storePinger    = { .what = "the store" };
static Pinger            troePinger     = { .what = "the temporal store" };

//
// The memory budget's figures, sampled by the port's own thread once a second - memoryBudgetValues
// reads /proc, which an answer does not
//
static uint64_t          memBudget      = 0;
static uint64_t          memUsed        = 0;
static uint64_t          memResident    = 0;
static uint64_t          memRefused     = 0;
static int64_t           memSampledMs   = 0;

//
// The requests in flight: a slot each, its request's state (corRestP) and when it started - so a
// finish is counted only for a request whose start was, and the age of the oldest is known.
//
// Not a pair of counters. Not every request the post-response hook sees went through the pre-dispatch
// hook - an accepted upgrade (WebSocket), a connection MHD completes without dispatching (the client
// gone mid-body) - and every one of those would move a started-minus-finished count for good: one way
// and a deadlock would hide, the other and an idle broker would be declared dead.
//
// A request that finds no free slot (more than 1024 in flight) is not tracked: neither its start nor
// its finish is counted.
//
static _Atomic(void*)    slotV[1024];
static _Atomic int64_t   slotStartMsV[1024];          // 0: being filled or emptied - not counted



// -----------------------------------------------------------------------------
//
// monotonicUs -
//
static int64_t monotonicUs(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}



// -----------------------------------------------------------------------------
//
// slotHome - where a request's state starts looking for its slot
//
static int slotHome(void* stateP)
{
  return (int) ((((uintptr_t) stateP >> 4) * 0x9E3779B97F4A7C15ULL) >> 54);   // the top 10 bits: 0 .. 1023
}



// -----------------------------------------------------------------------------
//
// healthPreDispatch - corNgsild's pre-dispatch first, then the request takes a slot
//
static void healthPreDispatch(void)
{
  preDispatchNext();

  void* stateP = corRestP;
  int   home   = slotHome(stateP);

  for (int ix = 0; ix < 1024; ix++)
  {
    int   slot  = (home + ix) & 1023;
    void* empty = NULL;

    if (atomic_compare_exchange_strong(&slotV[slot], &empty, stateP))
    {
      atomic_store_explicit(&slotStartMsV[slot], monotonicUs() / 1000, memory_order_relaxed);
      return;
    }
  }
}



// -----------------------------------------------------------------------------
//
// healthRequestEnd - its slot freed, and the request counted as finished - if it had one
//
void healthRequestEnd(void)
{
  if (enabled == false)
    return;

  void* stateP = corRestP;
  int   home   = slotHome(stateP);

  for (int ix = 0; ix < 1024; ix++)
  {
    int slot = (home + ix) & 1023;

    if (atomic_load_explicit(&slotV[slot], memory_order_relaxed) == stateP)
    {
      atomic_store_explicit(&slotStartMsV[slot], 0, memory_order_relaxed);
      atomic_store(&slotV[slot], NULL);
      atomic_store_explicit(&lastFinishedMs, monotonicUs() / 1000, memory_order_relaxed);
      atomic_fetch_add_explicit(&finished, 1, memory_order_relaxed);
      return;
    }
  }
}



// -----------------------------------------------------------------------------
//
// inFlightCount - how many requests hold a slot, and when the oldest of them started (0: none)
//
static int64_t inFlightCount(int64_t* oldestMsP)
{
  int64_t n      = 0;
  int64_t oldest = 0;

  for (int slot = 0; slot < 1024; slot++)
  {
    if (atomic_load_explicit(&slotV[slot], memory_order_relaxed) == NULL)
      continue;

    int64_t startedMs = atomic_load_explicit(&slotStartMsV[slot], memory_order_relaxed);

    n++;
    if ((startedMs != 0) && ((oldest == 0) || (startedMs < oldest)))
      oldest = startedMs;
  }

  *oldestMsP = oldest;
  return n;
}



// -----------------------------------------------------------------------------
//
// pingerLoop - a ping a second, bounded by 1 s; only a change of answer is logged
//
static void* pingerLoop(void* arg)
{
  Pinger* pP    = (Pinger*) arg;
  bool    wasOk = true;

  while (atomic_load(&stopping) == false)
  {
    int64_t t0 = monotonicUs();
    bool    ok = (pP->ping(1000) == 0);
    int64_t t1 = monotonicUs();

    atomic_store(&pP->latencyUs, t1 - t0);
    atomic_store(&pP->ok, ok);
    atomic_store(&pP->lastMs, t1 / 1000);

    if (ok != wasOk)
    {
      if (ok)
        COR_I("health: %s answers its ping again", pP->what);
      else
        COR_W("health: %s does not answer its ping", pP->what);
      wasOk = ok;
    }

    int64_t rest = 1000000 - (monotonicUs() - t0);
    if (rest > 0)
    {
      struct timespec ts = { rest / 1000000, (rest % 1000000) * 1000 };
      nanosleep(&ts, NULL);
    }
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// pingerStart - a thread for the store's ping, if it has one
//
static void pingerStart(Pinger* pP, int (*ping)(int timeoutMs))
{
  pP->ping = ping;

  if ((ping == NULL) || (atomic_load(&stopping) == true))
    return;

  if (pthread_create(&pP->tid, NULL, pingerLoop, pP) == 0)
    pP->running = true;
  else
    COR_W("health: no thread for the ping of %s - it is reported unreachable", pP->what);
}



// -----------------------------------------------------------------------------
//
// reachable - a store with a ping: answered within the last 5 s (a pinger stuck past its timeout goes
// stale); one without (in the broker's process): once loaded
//
// pP->ping is read only once 'loaded' is seen true: it is set before that flag (healthStoreLoaded).
//
static bool reachable(Pinger* pP, bool loaded, int64_t nowMs)
{
  if (loaded == false)
    return false;

  if (pP->ping == NULL)
    return true;

  int64_t lastMs = atomic_load(&pP->lastMs);

  return (atomic_load(&pP->ok) == true) && (lastMs != 0) && (nowMs - lastMs <= 5000);
}



// -----------------------------------------------------------------------------
//
// storeRender - a store's object in the report
//
static int storeRender(char* buf, int size, const char* plugin, Pinger* pP, bool loaded, int64_t nowMs)
{
  int n = snprintf(buf, size, "{\"plugin\":\"%s\",\"loaded\":%s,\"reachable\":%s",
                   (plugin != NULL) ? plugin : "none",
                   loaded ? "true" : "false",
                   reachable(pP, loaded, nowMs) ? "true" : "false");

  if (loaded && (pP->ping != NULL) && (n < size))
  {
    int64_t lastMs = atomic_load(&pP->lastMs);

    if (lastMs == 0)
      n += snprintf(&buf[n], size - n, ",\"lastPingAgoMs\":null,\"pingLatencyUs\":null");
    else
      n += snprintf(&buf[n], size - n, ",\"lastPingAgoMs\":%lld,\"pingLatencyUs\":%lld",
                    (long long) (nowMs - lastMs), (long long) atomic_load(&pP->latencyUs));
  }

  if (n < size)
    n += snprintf(&buf[n], size - n, "}");

  return n;
}



// -----------------------------------------------------------------------------
//
// reportRender - the JSON report into buf; *liveP and *readyP: what /live and /ready answer
//
static int reportRender(char* buf, int size, bool* liveP, bool* readyP)
{
  static const char* levelName[] = { "ok", "soft", "hard" };

  int64_t nowMs    = monotonicUs() / 1000;
  bool    isUp     = atomic_load(&serving);
  bool    sLoaded  = atomic_load(&storeLoaded);
  bool    tLoaded  = atomic_load(&troeLoaded);
  bool    storeOk  = reachable(&storePinger, sLoaded, nowMs);
  bool    troeOk   = reachable(&troePinger, tLoaded, nowMs);
  int     level    = memoryBudgetLevel();
  int64_t fin      = atomic_load_explicit(&finished, memory_order_relaxed);
  int64_t lastFin  = atomic_load_explicit(&lastFinishedMs, memory_order_relaxed);
  int64_t quietMs  = nowMs - ((lastFin != 0) ? lastFin : startMs);
  int64_t oldestMs;
  int64_t inFlight = inFlightCount(&oldestMs);
  bool    stalled;
  const char* status;

  //
  // Stalled: a request has been in flight for longer than --healthStallTimeout, and in all that time
  // none has finished. Both: a broker idle for an hour has 'none finished' the moment a request lands,
  // and one slow request (a large query, a slow forward) among requests that keep finishing is load.
  //
  stalled = (oldestMs != 0) && (nowMs - oldestMs > stallMs) && (quietMs > stallMs);

  *liveP  = (stalled == false);
  bool isStopping = atomic_load(&stopping);

  *readyP = isUp && storeOk && troeOk && (level < 2) && (stalled == false) && (isStopping == false);

  if (isStopping)                      status = "stopping";
  else if (isUp == false)              status = "starting";
  else if (stalled || !storeOk)        status = "down";
  else if (!troeOk || (level > 0))     status = "degraded";
  else                                 status = "ok";

  int n = snprintf(buf, size, "{\"status\":\"%s\",\"uptime\":%lld,\"store\":", status, (long long) ((nowMs - startMs) / 1000));

  if (n < size) n += storeRender(&buf[n], size - n, db.alias, &storePinger, sLoaded, nowMs);
  if (n < size) n += snprintf(&buf[n], size - n, ",\"troe\":");
  if (n < size) n += storeRender(&buf[n], size - n, troe.alias, &troePinger, tLoaded, nowMs);

  if (n < size)
    n += snprintf(&buf[n], size - n,
                  ",\"memory\":{\"budget\":%llu,\"used\":%llu,\"resident\":%llu,\"level\":\"%s\",\"refused\":%llu}",
                  (unsigned long long) memBudget, (unsigned long long) memUsed, (unsigned long long) memResident,
                  levelName[level], (unsigned long long) memRefused);

  if (n < size)
    n += snprintf(&buf[n], size - n, ",\"requests\":{\"inFlight\":%lld,\"total\":%lld,\"lastFinishedAgoMs\":",
                  (long long) inFlight, (long long) fin);

  if (n < size)
    n += (lastFin != 0) ? snprintf(&buf[n], size - n, "%lld}}", (long long) (nowMs - lastFin)) : snprintf(&buf[n], size - n, "null}}");

  return (n < size) ? n : size - 1;
}



// -----------------------------------------------------------------------------
//
// connectionServe - read one request line, answer it, close
//
// The request is read up to the blank line that ends its headers (all of them read, so the close is
// a FIN and not a reset that could overtake the answer), within 1 s and 2 KiB. Nothing read: a TCP
// probe, closed unanswered. Not "<method> <path> HTTP/1.x": closed unanswered too.
//
static void connectionServe(int fd)
{
  char    req[2048];
  int     len      = 0;
  int64_t deadline = monotonicUs() / 1000 + 1000;

  req[0] = 0;
  while ((len < (int) sizeof(req) - 1) && (strstr(req, "\r\n\r\n") == NULL))
  {
    struct pollfd pfd  = { fd, POLLIN, 0 };
    int64_t       left = deadline - monotonicUs() / 1000;

    if (left <= 0)
      break;

    int n = poll(&pfd, 1, (int) left);
    if ((n < 0) && (errno == EINTR))
      continue;
    if (n <= 0)
      break;

    n = recv(fd, &req[len], sizeof(req) - 1 - len, 0);
    if (n <= 0)
      break;

    len     += n;
    req[len] = 0;
  }

  char* eol  = strchr(req, '\n');
  char* path = strchr(req, ' ');
  char* end  = (path != NULL) ? strchr(path + 1, ' ') : NULL;

  if ((eol == NULL) || (end == NULL) || (end > eol) || (strncmp(end + 1, "HTTP/1.", 7) != 0))
    return;

  path++;
  *end = 0;
  path[strcspn(path, "?")] = 0;

  char body[1024];
  bool live;
  bool ready;
  int  bodyLen = reportRender(body, sizeof(body), &live, &ready);
  bool ok      = (strcmp(path, "/live") == 0) ? live : (strcmp(path, "/ready") == 0) ? ready : true;

  char resp[1400];
  int  respLen = snprintf(resp, sizeof(resp),
                          "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
                          ok ? "200 OK" : "503 Service Unavailable", bodyLen, body);

  if (respLen >= (int) sizeof(resp))
    respLen = sizeof(resp) - 1;

  send(fd, resp, respLen, MSG_NOSIGNAL);               // a client gone: no SIGPIPE, nothing to do
  shutdown(fd, SHUT_WR);
}



// -----------------------------------------------------------------------------
//
// serverLoop - one connection at a time; the memory figures sampled between them, once a second
//
static void* serverLoop(void* arg)
{
  (void) arg;

  while (true)
  {
    int64_t nowMs = monotonicUs() / 1000;

    if (nowMs - memSampledMs >= 1000)
    {
      memoryBudgetValues(&memBudget, &memUsed, &memResident, &memRefused);
      memSampledMs = nowMs;
    }

    struct pollfd pfd = { listenFd, POLLIN, 0 };
    if (poll(&pfd, 1, 1000) <= 0)
      continue;

    int fd = accept(listenFd, NULL, NULL);
    if (fd < 0)
      continue;

    struct timeval sendTimeout = { 1, 0 };            // a client that does not read cannot hold the port
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));

    connectionServe(fd);
    close(fd);
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// listenOpen - IPv6 and IPv4 on one socket where the host has IPv6, else IPv4
//
static int listenOpen(unsigned short port)
{
  int on  = 1;
  int off = 0;
  int fd  = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);

  if (fd >= 0)
  {
    struct sockaddr_in6 sa = { .sin6_family = AF_INET6, .sin6_port = htons(port), .sin6_addr = in6addr_any };

    setsockopt(fd, SOL_SOCKET,   SO_REUSEADDR, &on,  sizeof(on));
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,  &off, sizeof(off));

    if ((bind(fd, (struct sockaddr*) &sa, sizeof(sa)) == 0) && (listen(fd, 16) == 0))
      return fd;

    close(fd);
  }

  fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };

  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  if ((bind(fd, (struct sockaddr*) &sa, sizeof(sa)) == 0) && (listen(fd, 16) == 0))
    return fd;

  close(fd);
  return -1;
}



// -----------------------------------------------------------------------------
//
// healthStart -
//
bool healthStart(unsigned short port, int stallSecs)
{
  if ((listenFd = listenOpen(port)) < 0)
    return false;

  stallMs = stallSecs * 1000;
  startMs = monotonicUs() / 1000;

  //
  // Chained on the pre-dispatch hook in place (corNgsild's - it resets the request's NGSI-LD state):
  // the one moment every dispatched request passes - HTTP, cor://, a self-forward
  //
  preDispatchNext = corRestPreDispatchHookGet();
  corRestSetPreDispatchHook(healthPreDispatch);
  enabled = true;

  pthread_t tid;
  if (pthread_create(&tid, NULL, serverLoop, NULL) != 0)
    return false;
  pthread_detach(tid);

  COR_I("health port %u: /live, /ready and the report", port);
  return true;
}



// -----------------------------------------------------------------------------
//
// healthStoreLoaded -
//
void healthStoreLoaded(void)
{
  if (enabled == false)
    return;

  pingerStart(&storePinger, db.ping);
  atomic_store(&storeLoaded, true);       // after the ping is set: the server thread reads it only then
}



// -----------------------------------------------------------------------------
//
// healthTroeLoaded -
//
void healthTroeLoaded(void)
{
  if (enabled == false)
    return;

  pingerStart(&troePinger, troe.ping);
  atomic_store(&troeLoaded, true);
}



// -----------------------------------------------------------------------------
//
// healthServing -
//
void healthServing(void)
{
  atomic_store(&serving, true);
}



// -----------------------------------------------------------------------------
//
// healthStopping -
//
void healthStopping(void)
{
  atomic_store(&stopping, true);
}



// -----------------------------------------------------------------------------
//
// healthStop -
//
void healthStop(void)
{
  atomic_store(&stopping, true);

  if (storePinger.running)
    pthread_join(storePinger.tid, NULL);
  if (troePinger.running)
    pthread_join(troePinger.tid, NULL);

  storePinger.running = false;
  troePinger.running  = false;
}
