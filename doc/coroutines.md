# Coroutines - design

*Status: steps 1-3 built (2026-10-02) - the switch, the one wait, the cor:// server; § 8 has what they
measured. Short-term roadmap step 2 ("2x part II").*

## 1. Why

A request that **waits** - a distributed operation forwarded to a context source, an `@context`
that is not cached yet, a notification to send - cannot run on the event loop that read it, because
while it waits the loop serves nobody. So it takes the **thread hop**: handed to a worker thread,
processed there, handed back to be sent (`doc/performance.md`, "The thread hop"). The hop costs two
thread switches, which on one core is more than a cheap request costs, and a worker that waits on a
socket is a whole thread doing nothing.

Part I removed the hop for requests that **cannot** wait. Coroutines remove it for the ones that
can: the request runs on the loop that read it, as a coroutine with a stack of its own. Where it
would block on a socket, it gives the loop the socket to watch and **yields**; the loop goes on
serving everything else, and resumes the coroutine when the socket is ready. One thread, any number
of requests waiting at once, no thread switch anywhere.

Two things come with it:

- **cor:// multiplexing, for free.** On threads it cost 5-11 % of cor:// throughput, because every
  design paid a thread hand-off per request (`doc/cor-protocol.md` § 5.3, and the parked drafts
  coraine#207, corRest#23, corNgsild#55, corAlloc#5). With coroutines, many requests in flight on one
  connection and one thread is simply what the loop does.
- **Concurrent fan-out.** A distributed operation starts every forward, then waits for all of them -
  each a socket the loop watches. The drafts' `corRestCorStart` / `corRestCorWait` and their two
  functests carry over.

## 2. Decisions taken (KZ, 2026-10-02)

| | |
|---|---|
| Switch | **our own**, a few dozen lines of assembly per architecture (x86-64, aarch64 - the ARM image), in corBase. No dependency, no signal-mask system call per switch (`swapcontext` makes one). |
| Servers | **the builtin HTTP server (corHttp) and the cor:// server** first - both are our own epoll loops. libmicrohttpd keeps the worker hop for now. |
| mongoc | **workers, as today.** A MongoDB call blocks inside libmongoc, which a coroutine cannot yield out of. A yielding mongoc stream (`mongoc_client_set_stream_initiator`) is a later step. |

## 3. The model

**One scheduler per event loop**, in the loop's thread. The loop's `epoll_wait` already watches the
connections; it now also watches the sockets of suspended coroutines, and their timeouts.

**A request becomes a coroutine** when the inline check (`corRestAsyncDispatch`) says it could
wait - where today it is handed to a worker. A request that cannot wait still runs plainly on the
loop, as now: no coroutine, no stack, no cost.

**Yielding happens at one place.** Every wait in the clients today is a `poll()` on one socket (the
HTTP client, TLS, the cor:// client) or an `epoll_wait()` on the multi client's private set. They
all become one primitive:

```c
int corRestWaitFd(int fd, short events, int timeoutMs);   // like poll() on one fd
```

Inside a coroutine it registers `fd` with the loop's epoll, arms the timeout and yields; the loop
resumes the coroutine when the fd is ready or the time is up. Outside one - a worker, a background
thread, a libmicrohttpd thread - it is the `poll()` of today. **No caller changes**: the clients call
it where they called `poll()`, and the multi client waits on its own epoll fd (an epoll fd is itself
pollable). Notifications, `@context` downloads, distributed operations and cor:// forwards all go
through these clients, so all of them yield.

**Switching rebinds the request.** All request state is already reached through one thread-local
pointer, `corRestP` (part I made processing off the I/O thread possible that way), and corNgsild's
state hangs off `corRest.userData`. The scheduler sets `corRestP` to the coroutine's request before
it resumes it, and back after it yields - so `corRest` and `corNgsild` always mean the running
request, as they do on a worker today.

## 4. What must change, besides the scheduler

**The thread-local audit.** The invariant in our notes: per-request state in `__thread` is only
safe with one request per thread. `corRestP` and `corNgsild` are covered by the rebind. The rest of
the `__thread` variables, each to be checked - safe if it is never live across a yield:

| where | what | |
|---|---|---|
| corRest `corRestSelfForwardDepth` | recursion guard for in-process forwards | must follow the request, not the thread |
| corRest `clientConnV` (cor:// client) | per-thread connections | replaced by the shared, multiplexed connections of the drafts |
| corNgsild `ldOrderSort` statics, `ldDistOp` buffer | scratch during one call | safe - no yield inside |
| coraine `postEntities`, `bridgeSampleIn`, `bridgeServiceSync`, `mongocDotEscape` | static scratch buffers | check each: safe if consumed before the next wait |
| corDB `distDescCand` | query-time flag | check |
| timescale `timescaleConn` | a connection per thread | TRoE timescale stays off the coroutine path (blocking, like mongoc) |

**The `@context` cache** names a download's owner by `pthread_self()`, to tell a cyclic `@context`
from another thread's download. Two coroutines on one thread would look like one owner: the second
would be told its `@context` is cyclic. The owner becomes the request (the coroutine). And a request
waiting for another's download polls the cache with `usleep(20 ms)` - that becomes a yielding sleep.

**Never yield holding a lock.** A coroutine that yields with a mutex held, and another coroutine on
the same thread that wants it, deadlock: the holder cannot run. Every wait point is checked for
locks held across it (the client connection pools, the cor:// connection mutexes, corDB's lock -
which no wait is under today). A debug build asserts it: a per-thread count of locks held, checked
at every yield.

**`getaddrinfo` blocks**, and has no fd to wait on. Endpoints are resolved once and cached (with a
time to live); a miss resolves on a small resolver thread while the coroutine waits on an eventfd.

## 5. Stacks

Each coroutine gets a stack of its own, from a **pool per loop** (a finished coroutine's stack goes
back to the pool; no `mmap` per request in the steady state).

- `mmap`ed with a **guard page** below it: an overflow is a clean SIGSEGV on the guard, which the
  crash report names, not silent corruption of the next stack.
- **256 KiB of address space, committed lazily**: only the pages a request touches cost RAM - a few
  KiB for most. Deep recursion (a deeply nested JSON body) has room.
- **A cap per loop** (configurable, e.g. 1024 coroutines): beyond it a request that could wait takes
  the worker hop, as today. That is also the hook for the memory-budget / admission-control work.

## 6. Order of work

1. **The switch and the scheduler** in corBase - `corCoCreate / corCoResume / corCoYield`, the stack
   pool. Unit-tested on its own, both architectures (aarch64 under QEMU).
2. **`corRestWaitFd`** in corRest, every client wait moved onto it (`poll()` behaviour unchanged
   outside a coroutine - the whole functest suite must not notice).
3. **The cor:// server** runs waiting requests as coroutines; the thread-local audit; the `@context`
   cache owner. Measure.
4. **The builtin HTTP server** likewise. Measure.
5. **Multiplexing** rebuilt on the coroutines, from the drafts: the cor:// client's shared
   connections, `corRestCorStart/Wait`, the concurrent fan-out; their two functests.
6. **PGO** at the end of the step (roadmap): a profile-guided build, measured.

## 7. What it has to beat

The same measurements as before, release builds, deep C-states, back to back with the binary of
today (`test/perf/corChain.sh`, two rounds):

| three-broker chain, small entity, 16 callers | today (req/s) |
|---|---:|
| cor:// end to end | 67 700 |
| HTTP client, cor:// between the brokers | 42 300 |
| HTTP all the way | 23 900 |

with 1 caller, cor:// end to end: 11 600 req/s, p50 ~95 us. And `doc/performance.md`'s per-core
table for a single broker (inline vs hop), where a request that waits should now run at the inline
rate plus its own wait, not at the hop rate. **The bar: no measured loss anywhere** - a request that
does not wait must not notice the coroutines exist.

## 8. Steps 1-3: built, and measured (2026-10-02)

**Step 1** (corBase `corCo`): the switch as file-level assembly per architecture in `corCo.c`, stacks
of 256 KiB with a guard page from a pool per thread. A yield + resume is ~23 ns, a coroutine created,
run and finished ~29 ns. Tested on x86-64 and on aarch64 under QEMU (`make arm64-test` in corBase).

**Step 2** (corRest `corRestWaitFd`): every client wait in one place; the whole suite ran unchanged
through it.

**Step 3** (the cor:// server): a request that can wait runs as a coroutine of the loop that read it,
instead of moving its connection to a thread of its own. What else it took, from § 4:

- the cor:// client's per-thread connections are marked **busy** while a request uses one - two
  coroutines of a thread never share one - and **busy before the connect**: the HELLO exchange
  yields, and a connection already named for the peer was taken by another coroutine with its tables
  not yet open. That crashed broker A under 16 clients; the functests, which never run that many at
  once, did not see it - the benchmark did.
- the in-process forward's depth guard belongs to the request (`CorRestState`), not the thread
- the `@context` cache: the owner of a download is the request (the coroutine) and the wait for
  another's download yields (corJsonld `corLdConcurrencySet`). With the thread as owner, the second of
  two requests on one loop needing the same uncached `@context` was told it was cyclic - 400
  (`cor_coroutine_context_download_shared` fails that way without it)
- one `epoll_ctl` per wait: a socket stays in the loop's set between waits, disarmed

The three-broker chain, cor:// end to end, release builds, deep idle states, back to back with the
code before (`corChain.sh`, two rounds):

| | before | coroutines | |
|---|---:|---:|---:|
| small entity, 16 callers, req/s | 68 500 / 67 800 | **79 700 / 83 500** | +19-23 % |
| small entity, 16 callers, p99 | 377 µs | **226 µs** | -40 % |
| 20 attributes, 16 callers, req/s | 41 900 / 41 800 | **47 000 / 47 400** | +13 % |
| 20 attributes, 16 callers, p99 | 617 µs | **414 µs** | -33 % |
| small entity, 1 caller, req/s | 10 800 / 11 400 | 10 500 / 10 600 | -4 % (p50 91 -> 94 µs) |
| 20 attributes, 1 caller, req/s | 6 490 / 6 400 | 6 330 / 6 440 | -1 % (p50 157 -> 160 µs) |

With an HTTP client in front (broker A's front end is HTTP, unchanged) the chain is where it was.

**The one-caller cost** - ~3 µs over four hops, ~1 µs a server - is the connection's wait: before, a
connection waited in a blocking `read` on its own thread; now it waits in the loop's epoll, and is
re-armed (`epoll_ctl`) after every request. Multiplexing (step 5) keeps a connection armed for good -
the loop reads it while its requests run - which takes that call away.

**Not yet on the coroutine path** at step 3: TLS on a blocking socket waits inside OpenSSL; a write on
a full send buffer blocks; a new cor:// connection's `connect` and `getaddrinfo` block (once per
connection). None of them shows on the chain; all of them moved onto the loop in step 4.

## 9. Step 4: the builtin HTTP server (2026-10-02)

**One scheduler for both servers.** The cor:// server's scheduler moved to corBase as `corCoLoop`: a
wait hands its socket (or, with no socket, a deadline) to the thread's epoll set and yields; the loop
resumes the coroutine when the socket is ready or the deadline passes. A loop - corHttp's or the
cor:// server's - calls `corCoLoopEvent` for each event (a waiting coroutine's are tagged, the loop's
own are not), takes its `epoll_wait` timeout from `corCoLoopTimeoutMs` and expires the deadlines with
`corCoLoopExpire`. corRest binds it to the request (`corRestCoLoopInit`): `corRestP` is kept across a
yield and unbound when the loop resumes something else.

**The builtin server** suspends a request that can wait (the same `corRestAsyncDispatch` decision as
before), runs it as a coroutine of its loop, and resumes the connection on the loop thread when the
coroutine is done (`corHttpResumeHere` - no eventfd round trip; the worker hop keeps its own path).
Past the cap (1024 a loop), or with no stack to be had, the request takes the worker hop as before.

**The clients never block a loop:** sockets stay non-blocking; a write on a full send buffer waits
for POLLOUT; TLS loops on `SSL_ERROR_WANT_READ/WRITE` through `corRestWaitFd` (and a read checks
`SSL_pending` before it waits); a cor:// connect is non-blocking; a name that needs a lookup (not
numeric, not localhost) is resolved on a resolver thread while the coroutine waits on an eventfd
(`corRestResolve`).

**Which requests may be coroutines is the application's call** (`CorRestCoroutineHook`; with no hook,
none are). coraine says yes only with corDB, with TRoE none or corDB, and not for a PATCH that waits
for a bridge service (`?ddsSync`): mongoc, TimescaleDB and the bridge wait all block in a library
that knows nothing of the loop, and a blocked loop stops every connection on it. The suite found that
one: `bridge_service_sync_cap` with the ddsSync wait on a coroutine.

**Found on the way:**

- the builtin server took ~1 s to stop - its loop only saw the stop flag at its 1 s epoll timeout;
  `corHttpStop` now wakes it (7 ms with corDB)
- inline dispatch over cor:// never saw an `application/ld+json` body's `@context`: the body arrives
  parsed (`requestTree`) and there was no text to scan, so a body whose `@context` had to be
  downloaded ran inline. Old as cor:// itself; found by the first suite run in cor mode with corDB
  (`inline_dispatch`)
- a coroutine that never waits (a batch update with corDB) finishes before the server's callback
  returns, and its answer went out twice - the second an empty one, which a keep-alive client took
  for the answer to its next request: half of `perfRun.sh`'s batch updates came back as errors. Not a
  functest had seen it - curl reads one answer and hangs up - and not the chain, where every
  coroutine waits for its forward. `http_keepalive_coroutine_no_wait` sends three on one connection

**The suites** (local, 2026-10-02): builtin + corDB 676/676, builtin + mongoc 731/731, libmicrohttpd
+ mongoc 733/733, cor mode + libmicrohttpd + corDB 678/678 (with the inline-dispatch fix).

**Measured** (`doc/performance.md`, "A request that waits"): the three-broker chain on the built-in
server, HTTP all the way +16-24 %, cor:// between the brokers +14-33 %; one broker on one core, the
batches +7-36 % (a coroutine instead of the hop); libmicrohttpd and `mongoc`, which keep the hop,
unchanged; a request that never waited costs the same instructions it did.

### 9.1 Sixteen callers: the p99 that doubled, and what it was

The first measurement of the chain (HTTP all the way, builtin server) had the one-caller numbers up
by half and the sixteen-caller p99 **doubled** - 1.0-1.5 ms before, 2.3-2.9 ms after - in a closed
loop (wrk: each connection sends its next request when the answer is in).

**The loops were unevenly loaded.** A broker on two cores runs two loops, each with its own listen
socket on the port (`SO_REUSEPORT`), and the kernel hashes each new connection to one of them. With
a request now running on the loop that read it, the split is the work split - and 16 connections came
out as uneven as 361 / 751 CPU ticks over 8 s (800 = a whole core). Before, 32 workers time-sliced by
the kernel evened it out whatever the split.

**One accepting loop** (corHttp `corHttpAcceptShare`): loop 0 accepts every connection and deals them
out in turn, through each loop's own queue and eventfd; the others close their listeners. The split
became even (678 / 679), and the chain's throughput rose again, to ~+25 % over step 3 - but the
closed-loop p99 stayed where it was. The imbalance was real; it was not the tail.

**The tail was the load.** A closed loop runs each build at the rate it reaches: the faster build was
measured at 39 000 req/s with its loops at 85-92 % of a core, the old one at 31 800 with room to spare.
At one and the same rate (corTools `corRequest --rate`, latency counted from when a request was due),
16 connections, two rounds:

| chain, HTTP, builtin | step 3 p50 | **step 4 p50** | step 3 p99 | **step 4 p99** |
|---|---:|---:|---:|---:|
| small entity @ 15 000 req/s | 185-198 µs | **178-206 µs** | 0.35-1.07 ms | 0.35-3.98 ms |
| small entity @ 20 000 req/s | 187-202 µs | **174-176 µs** | 0.32-2.17 ms | **0.23-1.85 ms** |
| small entity @ 25 000 req/s | 198-202 µs | **174-175 µs** | 0.80-19.4 ms | **0.27-0.81 ms** |
| 20 attributes @ 10 000 req/s | 433-454 µs | **317-355 µs** | 0.78-1.08 ms | **0.60-0.63 ms** |
| 20 attributes @ 15 000 req/s | 454-475 µs | **310-377 µs** | 1.14-2.38 ms | **0.55-0.65 ms** |

At the same load the p50 is lower everywhere and the p99 lower or equal; a p99 above a millisecond
is a coin toss between rounds for both builds (one round, one rate: 0.8 ms and 19 ms). Both fall off
the same cliff (small entity @ 30 000, 20 attributes @ 20 000 - p99s of 12-24 ms either way).

**So no yield budget** (a coroutine handing the loop back after so much work): nothing measured at
an equal load asks for one. The closed-loop p99 is still the number to watch - a loop at 90 % of a
core is one burst from queueing, where the old worker pool spread a burst over 32 threads.

## 10. The post-response phase (2026-10-02)

A write's deferred work - its notifications above all - ran after the response, on a worker: the
builtin server's loop could not run it (a notification may need an `@context` this broker serves, and
the loop would wait for an answer only it could give). The finish-inline hook already kept the phase
on the loop when it had nothing that could wait; now **the rest runs as a coroutine of the loop**
too (corRest `CorRestFinishCoroutineHook`), in both servers: it yields where it waits, and the loop
goes on - serving that `@context` request itself, if it comes to that.

What the phase can wait for, and how each yields:

| | |
|---|---|
| notification, HTTP | corRest's client - `corRestWaitFd` |
| notification, any other scheme (`mqtt://`) | the bridge plugin's own client blocks (mosquitto): corNgsild runs it through corBase's **`corCoBlocking`** - on a thread of its own, the coroutine waiting on an eventfd |
| CSR notification, the registration probe | corRest's client |
| expired entities, TRoE | in-process with corDB - no wait |
| a bridge goal's release | waits on the bridge - **stays on a worker** (coraine's hook says no) |

No lock is held across any of those sends - each pins what it reads and sends unlocked. coraine says
yes with corDB and TRoE none/corDB; mongoc keeps the worker. Where the response went out from inside a
request's coroutine (the builtin server answers in `corHttpResumeHere`), the phase is a coroutine
started from it - a coroutine that waits yields back to the one that resumed it, and the loop resumes
it later.

`http_coroutine_post_response_phase`: one worker, a notification receiver that takes 1.5 s, three
writes - on the worker, one notification had reached the receiver a second later; as coroutines,
all three.

## 11. Open questions

- The cap and the stack size as options, or fixed? (`--coroutines`, `--coStack`?)
- libmicrohttpd: worth its `MHD_suspend/resume_connection` later, or does the builtin server become
  the default HTTP server instead?
- The yielding mongoc stream: when?
