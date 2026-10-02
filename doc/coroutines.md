# Coroutines - design

*Status: proposal, for review. Short-term roadmap step 2 ("2x part II").*

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

## 8. Open questions

- The cap and the stack size as options, or fixed? (`--coroutines`, `--coStack`?)
- libmicrohttpd: worth its `MHD_suspend/resume_connection` later, or does the builtin server become
  the default HTTP server instead?
- The yielding mongoc stream: when?
