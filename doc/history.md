# History

How coraine got where it is: designs tried and dropped, regressions and how they were found, the
before-and-after of a change. The other documents say what the software **is** and what it measures
**now**; this one keeps the story, for whoever wants it. Newest first.

## 2026-10-02/03 - coroutines, steps 1 to 6

Requests that wait moved off worker threads onto the event loops, in six steps: the switch (corBase
`corCo`), one wait primitive (`corRestWaitFd`), the cor:// server, the built-in HTTP server and the
post-response phase, cor:// multiplexing, and PGO. What follows is the design document as it was
written and extended while building - its plans, its measurements against the build before each
step, and what each step found on the way. What the coroutines are today is in `doc/coroutines.md`.


*Status: steps 1-3 built (2026-10-02) - the switch, the one wait, the cor:// server; § 8 has what they
measured. Short-term roadmap step 2 ("2x part II").*

#### 1. Why

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

#### 2. Decisions taken (KZ, 2026-10-02)

| | |
|---|---|
| Switch | **our own**, a few dozen lines of assembly per architecture (x86-64, aarch64 - the ARM image), in corBase. No dependency, no signal-mask system call per switch (`swapcontext` makes one). |
| Servers | **the builtin HTTP server (corHttp) and the cor:// server** first - both are our own epoll loops. libmicrohttpd keeps the worker hop for now. |
| mongoc | **workers, as today.** A MongoDB call blocks inside libmongoc, which a coroutine cannot yield out of. A yielding mongoc stream (`mongoc_client_set_stream_initiator`) is a later step. |

#### 3. The model

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

#### 4. What must change, besides the scheduler

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

#### 5. Stacks

Each coroutine gets a stack of its own, from a **pool per loop** (a finished coroutine's stack goes
back to the pool; no `mmap` per request in the steady state).

- `mmap`ed with a **guard page** below it: an overflow is a clean SIGSEGV on the guard, which the
  crash report names, not silent corruption of the next stack.
- **256 KiB of address space, committed lazily**: only the pages a request touches cost RAM - a few
  KiB for most. Deep recursion (a deeply nested JSON body) has room.
- **A cap per loop** (configurable, e.g. 1024 coroutines): beyond it a request that could wait takes
  the worker hop, as today. That is also the hook for the memory-budget / admission-control work.

#### 6. Order of work

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

#### 7. What it has to beat

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

#### 8. Steps 1-3: built, and measured (2026-10-02)

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

#### 9. Step 4: the builtin HTTP server (2026-10-02)

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

##### 9.1 Sixteen callers: the p99 that doubled, and what it was

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

#### 10. The post-response phase (2026-10-02)

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

**Measured** (one broker on one core, builtin server, corDB, a subscription matching every write,
the receiver on other cores; `doc/performance.md`): PATCH with 50 connections 28 900 -> **45 000**
req/s (+58 %), p99 3.7 -> **1.1 ms**; with one connection 29 100 -> **38 800** (+33 %), p99 45 ->
**29 µs**. On the worker the phase cost two thread switches per write and queued behind the pool;
on the loop it is one coroutine and one send.

On the way: the benchmark's first receiver - a debug corTestClient - wrote a full trace of every
request, 25 GB of `/tmp` in minutes, and could not keep up: it made the coroutines look 30 % *slower*
at one connection. corTestClient `--traceLevels ""` for a receiver that measures nothing of its own.

#### 11. Step 5: multiplexing (2026-10-02)

Many cor:// requests in flight on one connection - how it works is in `doc/cor-protocol.md` § 5.3; on
threads it had cost 5-11 % and was dropped, on the coroutines it costs nothing. What it took here:

- corBase `corCoLoopPark` / `corCoLoopWake`: a coroutine waits until another of its loop wakes it -
  the reader of a connection waking the coroutine whose response it decoded, a writer the next one
  queued for the connection's write turn
- the server armed once per connection instead of once per request (`EPOLLONESHOT`, re-armed after
  every response - § 8 put step 3's one-caller cost on it). Gone now, and the one-caller chain did not
  move for it (14 400 req/s either way): the re-arm was not where those microseconds went. It
  reassembles a next frame only from bytes already read - a `read()` that finds nothing is the epoll's
  to report
- the client reads into its buffer as soon as the socket is ready: waiting for readiness and then
  again inside `connRead` cost an `epoll_ctl` and a round of the loop per response, and the first
  measurement had one caller 3 % down for it
- corRequest `--sockets 1`: every path on one connection, printed in the order the answers came

**Measured** - the three-broker chain, release builds, deep idle states, two rounds, back to back with
the build before (`corChain.sh`):

| | before | **multiplexed** |
|---|---:|---:|
| cor:// end to end, small entity, 16 callers | 67 547 / 79 159 | **83 400 / 83 832** (p99 271 / 260 µs) |
| cor:// end to end, small entity, 1 caller | 14 514 / 14 364 | 14 388 / 14 384 |
| cor:// end to end, 20 attributes, 16 callers | 45 849 / 42 804 | 44 867 / 44 963 |
| HTTP client, cor:// between, small entity, 16 callers | 67 310 / 62 292 (p99 3.0 / 3.2 ms) | **73 466 / 74 195 (p99 441 / 416 µs)** |
| HTTP client, cor:// between, 20 attributes, 16 callers | 35 950 / 36 194 (p99 3.3 ms) | 37 868 / 37 442 (p99 0.94 ms) |

One caller, 20 attributes, cor:// end to end moved ±5 % between runs either way; `perf stat` over the
three brokers settles it: **1 251 807 / 1 250 186 instructions and 548 840 / 545 107 cycles a
request**, before / after - the same. The p99 before at 16 callers was the client running out of
connections: eight per thread, and a request finding them all busy opened one of its own, HELLO
included. A fan-out to two cor:// sources of one second each: two seconds before, one now
(`cor_fanout_concurrent`).

#### 12. Step 6: PGO (2026-10-02)

`make pgo` (`doc/building.md`, numbers in `doc/performance.md` "Profile-guided"): every library and the
broker built instrumented, trained on `test/perf/pgoTrain.sh`, rebuilt with the profile. Against the
same source without it: +1-10 % per core, +4-15 % on the three-broker chain, nothing slower - unlike
LTO (2026-09-30), which was noise. Two details it needed: `-fprofile-update=atomic`, the broker being
multi-threaded; and the libraries under corRest keep one archive whatever the flavour, so the target
rebuilds them as debug at its end - a `make di` after it would otherwise link profile-guided release
archives (`doc/performance.md`, "Every library of a release broker as release").

The Docker image (release) and the nightly performance job build with it (KZ, 2026-10-03): what is
measured is what is shipped. `test/perf/pgoTrain.sh` pins nothing and runs its own three-broker chain,
so it trains inside a Docker build or on a 4-core runner as well as here; the builder stage has `wrk`.
`PGO_RESTORE_DEBUG=0` skips the closing debug rebuild where nothing builds debug afterwards. The
nightly history steps up on the night it started.

#### 13. Open questions

- The cap and the stack size as options, or fixed? (`--coroutines`, `--coStack`?)
- libmicrohttpd: worth its `MHD_suspend/resume_connection` later, or does the builtin server become
  the default HTTP server instead?
- The yielding mongoc stream: when?


### The performance page's account of steps 3-5, as it stood

##### A request that waits: a coroutine, not a hop

What still took the hop - a forward to another broker, an `@context` download -
did so only because it **waits**. With the coroutines (`doc/coroutines.md`) the
built-in server runs such a request as a coroutine of the loop that read it: it
yields where it waits, the loop serves the other connections meanwhile, and it
resumes on the same thread. The cor:// server has done that since step 3; the
built-in HTTP server since step 4. libmicrohttpd and `mongoc` keep the hop (a
MongoDB call blocks inside its driver, which knows nothing of the loop).

Three brokers chained, a GET on A forwarded to C through B (`test/perf/corChain.sh`),
each broker on two cores, `corDB`, release builds, deep idle states, two rounds,
the built-in server before (step 3) and after (step 4):

| A -> B -> C, built-in server | before | **after** | |
|---|---:|---:|---:|
| HTTP all the way, small entity, 1 caller | 6 643 / 6 572 | **8 628 / 7 753** | +24 % |
| HTTP all the way, small entity, 16 callers | 32 650 / 32 996 | **35 569 / 40 668** | +16 % |
| HTTP all the way, 20 attributes, 16 callers | 21 486 / 21 651 | **23 213 / 24 185** | +10 % |
| cor:// between the brokers, small entity, 1 caller | 8 794 / 8 884 | **12 945 / 9 927** | +28 % |
| cor:// between the brokers, small entity, 16 callers | 47 907 / 50 140 | **64 899 / 65 611** | +33 % |
| cor:// between the brokers, 20 attributes, 16 callers | 31 906 / 33 010 | **37 427 / 36 570** | +14 % |
| cor:// end to end | 84 267 / 85 233 | 83 313 / 84 275 | unchanged (step 3 already) |

*(req/s, 2026-10-02.)* **The sixteen-caller p99 went up** in this closed-loop
measurement - 0.95-1.4 ms before, 1.4-3.5 ms after - because each loop now does
the work the worker pool did, and the faster build is measured at a higher load:
at 40 000 req/s a loop runs at ~90 % of its core, where a burst queues. At **the
same rate** for both (corTools `corRequest --rate`, latency counted from when a
request was due) the after build has the lower p50 everywhere and the equal or
lower p99 - the table is in `doc/coroutines.md` § 9.1.

The loops share one accepting loop now (corHttp `corHttpAcceptShare`): left to
the kernel's `SO_REUSEPORT` hash, 16 connections split as unevenly as 11 and 5
between two loops, and the busier one queued.

**One broker, one core** (`perfRun.sh`, built-in server, `--httpLoops 1`): a
batch could always wait, so it took the hop; now it is a coroutine that does not
(with `corDB` it never yields), and the batches gain the most:

| One core, `corHttp` + `corDB` | before (two runs) | **after** | |
|---|---:|---:|---:|
| batch update, 20 entities | 8 467 / 8 313 | **11 418** | +36 % |
| batch create, 20 entities | 7 522 / 7 501 | **8 039** | +7 % |
| batch delete, 20 entities | 21 336 / 21 610 | **28 989** | +35 % |
| batch update, p99 | 108 ms | **4.5 ms** | |

*(requests/s, 2026-10-02.)* Everything that already ran inline came out 1-3 %
lower in that one run - and the same in instructions and cycles: `perf stat` on
a retrieve, 29 001 user-space and 21 890 kernel instructions a request after,
29 003 and 21 857 before, cycles within 0.2 %. Run-to-run noise, not a cost.

**A write that notifies** - its post-response phase, the notification above all, a coroutine of the
loop too (`doc/coroutines.md` § 10). One broker on one core, a subscription matching every PATCH,
the receiver (corTestClient, release, `--traceLevels ""`) on other cores:

| One core, `corHttp` + `corDB`, every write notifies | phase on a worker | **a coroutine** | |
|---|---:|---:|---:|
| PATCH, 50 connections | 28 909 / 28 071 | **45 074 / 44 958** | +58 % |
| p99 | 3.65 / 3.94 ms | **1.12 / 1.11 ms** | |
| PATCH, 1 connection | 29 266 / 28 925 | **38 712 / 38 826** | +33 % |
| p99 | 43 / 46 µs | **29 / 29 µs** | |

*(req/s, 2026-10-02; one notification per write reached the receiver in both.)*

**cor:// multiplexed** (`doc/cor-protocol.md` § 5.3, `doc/coroutines.md` § 11): many requests in
flight on one connection. The three-broker chain at 16 callers: cor:// end to end 67 500-79 200 ->
**83 400-83 800** req/s; HTTP in front and cor:// between 62 300-67 300 -> **73 500-74 200**, its p99
3.0-3.2 ms -> **0.42-0.44 ms**. One caller: unchanged, and so are the instructions per request.

**Nothing else moved.** libmicrohttpd before and after - every request shape on
one core, `corDB` and `mongoc`, and the chain - within ±5 % run to run (the clients
under it changed: non-blocking sockets, TLS on the loop, a resolver thread).



## 2026-10-02 - cor:// multiplexing, on threads and then on the coroutines

#### 5.3 Multiplexing - on the coroutines (2026-10-02)

Many requests in flight on one connection, answered in the order they finish: no byte of the format
changed - the frames carry correlation ids from the start.

**The one rule** any design has to keep: the tables follow the stream (§ 4.13) - frames are encoded
in the order they are sent and decoded in the order they arrive.

**Built on threads first, and dropped.** Every threaded design put a thread hand-off on a request's
path, and on two cores per broker CPU per request is the throughput: the best of them cost 5-11 % of
the three-broker chain (16 callers, `perf stat`, release builds):

| server | client | req/s | thread switches per request (broker A) |
|---|---|---:|---:|
| a thread per connection, one request in flight | a connection per calling thread | 74 000 | 1.6 |
| requests that wait handed to the worker pool | 2 shared connections per peer | 48 000 | 3.2 |
| same | an idle connection first, up to 16 | 57 000 | 2.7 |
| a thread owns the connection, the event loop takes what arrives meanwhile | same | 66 000 | 2.0 |

**Built on the coroutines** (`doc/coroutines.md` § 11), where many requests in flight on one thread is
simply what the event loop does:

- **server**: a connection is armed once, for good; the loop reads every frame that comes, decodes
  each request as it arrives (wire order), runs it - inline, or as a coroutine - and queues each
  response, encoded as it is queued (wire order again), flushed on `EPOLLOUT` when the socket is full.
  A request in flight holds a reference to the connection - one may finish on a worker.
- **client**: one connection a peer, per thread, shared by every request of the thread. A frame goes
  out under the connection's *write turn* (a coroutine that finds it taken parks); the first request
  that waits and finds no reader takes the *read turn*, decodes every frame as it comes into the memory
  of the call it belongs to, wakes that call's coroutine (corBase `corCoLoopPark/Wake`), and hands the
  turn on when its own response is in. A response nobody waits for any more is decoded all the same -
  the tables need it - and dropped.
- `corRestCorStart` / `corRestCorWait`: a request sent now, its response collected later. A
  distributed query fanned out to several cor:// sources starts every request before it waits for any.
- With `mongoc` a request that waits cannot be a coroutine (the driver blocks): its connection moves
  to a thread of its own, one request at a time, as before.

Two functests fail without it: `cor_multiplex_out_of_order` (a fast request answered before a slow one
sent earlier on the same connection) and `cor_fanout_concurrent` (two sources of one second each: two
seconds before, one now). Measured in `doc/performance.md` - no loss anywhere, the instructions per
request unchanged.


## 2026-10-01 - cor:// v1: the plan it was built to

### 6. Order of work

1. **The codec** in corTree, with the corNgsild callbacks (§ 4.1-4.3), round-trip exact.
2. **cor:// forwarding**, client and server in corRest, proven on a **chain of three brokers**:
   A and B each hold a registration pointing at the next, C holds the entity. A query to A is
   forwarded to B, then to C, and the entity comes back all the way. The same chain is run twice,
   once with `http://` registrations and once with `cor://`:
   - the answers must be identical, byte for byte - the functest
   - the difference in latency and throughput is the benchmark, and the § 4.12 sizes are measured
     on the same traffic
3. **The corDB snapshot** - the same codec to a file, and back on start.
4. **The corDB log** - the same records appended, replayed on start; history by retention.

#### 6.1 v1, measured (2026-10-01)

Steps 1 and 2 are built: the codec (corTree), its NGSI-LD callbacks (corNgsild), cor:// in corRest -
a listener (`--corPort`) and a client - and forwarding over it. `cor_forwarding_chain` and
`cor_api_direct` are the functests; `test/perf/corChain.sh` the benchmark: three corDB brokers chained
A -> B -> C, a GET on A, so every request crosses both hops twice. Release build, each broker on two
cores, two runs each (ranges):

| Entity | Mode | 1 conn req/s | p50 | p99 | 16 conns req/s | p50 | p99 |
|---|---|---|---|---|---|---|---|
| 4 attributes | http | 6,519-7,039 | 146 us | 1.1-1.2 ms | 23,515-23,595 | 624 us | 1.74 ms |
| 4 attributes | cor (hops) | 8,428-8,585 | 111 us | 0.2-0.7 ms | 40,912-44,887 | 337-362 us | 0.84-1.03 ms |
| 4 attributes | **cor-all** | **14,293-14,793** | **66 us** | **78-94 us** | **64,056-65,012** | **240-245 us** | **399-401 us** |
| 20 attributes | http | 3,848-4,030 | 250 us | 0.3-1.6 ms | 17,261-17,556 | 0.86 ms | 1.98-2.11 ms |
| 20 attributes | cor (hops) | 4,915-4,944 | 192 us | 1.4-2.3 ms | 28,233-28,310 | 537 us | 1.18-1.19 ms |
| 20 attributes | **cor-all** | **6,347-6,669** | **162 us** | **183-190 us** | **39,697-40,138** | **391-395 us** | **642-647 us** |

- **http** - every hop HTTP and JSON; the client is wrk
- **cor (hops)** - the two forwarding hops cor://, the client still wrk over HTTP
- **cor-all** - nothing but cor://: the client is `corRequest` in its load mode

All cor:// against all HTTP: **2.1-2.2x with one connection, 2.3-2.8x with sixteen**, p99 under load
4x lower for the small entity and 3x for the large; the single-connection p99 stops being noise
(78-190 us, where every run with an HTTP client leg swings between 0.2 and 2.3 ms).

**One broker, no forwarding** - what the transport itself costs, a GET on corDB:

| | 1 conn req/s | p99 | 16 conns req/s | p99 |
|---|---|---|---|---|
| HTTP (wrk) | ~41,000 | ~33 us | ~142,000 | 4-9 ms |
| cor:// (corRequest) | 43,400 | 31 us | 145,300 | 209 us |

At parity on throughput, with a tail an order of magnitude tighter under load. (The first cut was
well behind - 29,000 / 82,000 req/s: it polled before every read and write and read and sent the
header and the tree separately. Buffered reads and one send per frame took it from ~8 system calls a
request to ~2.)

Sizes on the wire: the generic codec gives 84 % of minimised JSON, with the NGSI-LD callbacks 71 %,
over 660 JSON documents of the ETSI suite and coraine's tests - payloads with compact names and
inline `@context` text, so the least favourable case; broker-to-broker traffic, with its expanded
names, is to be measured on the chain.

Since 2026-10-02 a request that waits runs as a coroutine of the server's event loop (`doc/coroutines.md`
§ 8): the chain end to end with 16 callers went from ~68 000 to ~80 000 req/s, its p99 from ~380 to
~225 µs.

Not yet: multiplexing (§ 5.3), and with it a fan-out to several cor:// sources that has them all in
flight at once; packed numeric arrays and timestamps as integers (§ 4.9, § 4.10).


## 2026-09-30 and later - measured, and dropped or found on the way

#### Tried, measured, and dropped

Not every idea that should make a broker faster does. Measured on the same
machine, release builds, and left out:

- **Link-time optimisation** (`-flto` across the libraries, 2026-09-30): noise.
  About three quarters of a small request's cycles are spent in the kernel - its
  system calls and thread switches - and LTO can only work on the quarter that is
  ours. It did find a real bug on the way: a `corAlloc()` result used unchecked
  in the JSON parser (fixed). Profile-guided optimisation, measured after it, is
  not noise - see "Profile-guided" above.
- **Compiling the traces out of release builds** (2026-09-30): about +1 % on
  writes, nothing measurable on queries. The cost of a disabled trace had already
  gone - a trace level is checked before any of its arguments are evaluated - so
  removing them was a decision about what a release ships (no trace code, and a
  crash report instead), not a speed-up.
- **Every library of a release broker as release** (2026-10-02): `make release`
  builds only corRest, corJsonld and corNgsild as release (`libs-release`); the
  libraries under them - corBase, corAlloc, corJson, corTree, corHttp and the
  rest - come in as whatever was built last, the debug build with its traces
  compiled in. All of them release: 0.2-0.8 % fewer instructions and cycles a
  request (`perf stat`: retrieve, PATCH, a 20-entity query). Not worth a number;
  worth fixing, because a release broker should be one. It needs a change in
  each of those libraries first - they keep ONE archive, and a `make di` after a
  release build leaves the release one in place (objects older than it) - one
  archive per flavour, as corRest, corJsonld and corNgsild have.
- **Multiplexing cor:// on threads** (2026-10-02): 5-11 % *less* cor://
  throughput, because every design put a thread hand-off on each request's path.
  It is deferred to the coroutines, where it costs nothing - the measurements are
  in `doc/cor-protocol.md` § 5.3.


## 2026-09-26/27 - a create regression, and how it was found


On 2026-09-26/27 the nightly performance run went red: creates had fallen 35 % with
`mongoc`. A merge of 09-25 had put a full retrieve of the entity before
**every** `POST /entities` - needed only when a bridge Channel sends the entity
on, done always. The gate only tripped on the second bad night (a single slow
run is noise to it), which is also how close a regression comes to being absorbed
into the median. It was bisected with the images every merge publishes - a broker
per merge, measured in quarters - and fixed by doing the retrieve only when a
Channel is there (coraine#171): `mongoc` creates back to ~9 400/s with 50
clients, confirmed by the next nightly.


## 2026-09-15 to 09-30 - writes: no locking, a quadratic batch create, deletes that walked the store

Half of what a context broker does is writes, and until 2026-09-15 this page
had no write number on it at all. That was also the half that was broken:
`corDB` had no locking whatsoever, and twenty concurrent PATCHes killed the
broker every time. A read-only benchmark could never have noticed.

It was not always. Until the commit that added the create benchmark, batch
create walked the entire entity list for every incoming entity — with the id
index sitting right there, maintained by the bottom of the same loop — and was
**6× slower per entity than creating them one at a time**. A batch create is
the one operation that grows the store it is scanning, so it got worse as it
ran. Nothing measured it, so nothing caught it.

Deletes had the same history. Until 2026-09-30 there was no delete scenario, and
`corDB` found an entity through its id index and then walked the store from the
first entity to find the one *before* it, to unlink it - under the write lock.
Deletes slowed down as the store grew, and a batch of twenty against 40 000
entities took up to 0.9 s at p99. The index now maps each id to the entity's
predecessor, so an unlink is O(1): single deletes 4.2× faster, batch deletes 60×.

## Measurement notes

- **The error guard in perfRun.sh** exists because a create scenario once reported 166 036 creates/s -
  faster than a PATCH, impossible - which turned out to be the speed of answering 409 to duplicate ids.
- **The `mongoc` RAM figure** fell from 202 to 68 MiB when the fixture moved from single POSTs to
  batches of 200: the measurement changed, not the broker.
- **`corHttp` against libmicrohttpd on one core** used to favour `corHttp`; by 2026-09 it was the other
  way round (5 901 against 6 588) - the `--connectionPoolSize` question in `doc/performance.md`.

