# Coroutines

A request that **waits** - a forward to another broker, an `@context` download, a notification to
send - runs as a **coroutine** of the event loop that read it. Where it would block on a socket it
hands the socket to the loop and yields; the loop serves every other connection meanwhile and
resumes the coroutine when the socket is ready. One thread, any number of requests waiting at once,
no thread switch.

How this came about - the design, the steps, what each one measured - is in `doc/history.md`.

## 1. Where a request runs

| request | runs |
|---|---|
| cannot wait (the entities family, `corDB`, everything it needs cached) | **inline**, on the loop that read it - no coroutine |
| can wait, on the built-in HTTP server or the cor:// server | **a coroutine** of that loop |
| can wait, on libmicrohttpd | a worker thread |
| any request with `mongoc`, TRoE `timescale`, or a PATCH that waits for a bridge service (`?ddsSync`) | a worker thread (HTTP); a thread of its own for the connection (cor://) |

The last row are waits inside a library that knows nothing of the loop - libmongoc, libpq, the
bridge's condition variable: a coroutine there would stop the loop for the length of the wait. The
application decides which requests may be coroutines (corRest `CorRestCoroutineHook`; no hook, no
coroutines). coraine says yes with `corDB` and TRoE `none` or `corDB`, except for `?ddsSync`.

**The post-response phase** - a write's notifications above all - runs the same way: inline when
the request left nothing that can wait, else as a coroutine (`CorRestFinishCoroutineHook`: `corDB`,
and no bridge goal release pending - that one waits on the bridge and goes to a worker).

**A cap of 1024 coroutines per loop.** Past it, or with no stack to be had, a request that can wait
goes to a worker (HTTP) or moves its connection to a thread of its own (cor://, when nothing else of
that connection is in flight; else it runs on the loop).

## 2. Waiting

Every wait of the clients goes through **one function**:

```c
int corRestWaitFd(int fd, short events, int timeoutMs, short* reventsP);   // as poll() on one fd
```

In a coroutine it registers `fd` with the loop's epoll (one `epoll_ctl`, `EPOLLONESHOT`), arms the
timeout and yields; elsewhere - a worker, a libmicrohttpd thread - it is `poll()`. Everything that
waits goes through it:

- **HTTP and cor:// clients**: non-blocking sockets; a full send buffer waits for `POLLOUT`
- **TLS**: `SSL_connect`, reads and writes retry on `SSL_ERROR_WANT_READ/WRITE`; a read checks
  `SSL_pending` before it waits
- **connect**: non-blocking, a wait for `POLLOUT`
- **name resolution** (`corRestResolve`): a name that is not numeric and not `localhost` is resolved
  on a thread of its own while the coroutine waits on an eventfd
- **a call that blocks in another library** (corBase `corCoBlocking`): run on a thread of its own,
  the coroutine waiting on an eventfd - notifications to `mqtt://` and other non-HTTP schemes, sent
  by a bridge plugin's client, go this way
- **an `@context` another request is downloading**: the waiting request yields until it is cached;
  a download's owner is the request, not the thread, so two coroutines of one thread needing the
  same `@context` are not taken for a cycle

**Request state across a yield.** All of a request's state is reached through one thread-local
pointer, `corRestP`; it is saved before a wait and restored after it, and the loop unbinds it when a
coroutine yields back (`corRestCoLoopInit`).

**No lock is held across a wait.** A coroutine that yielded with a mutex held would deadlock the next
coroutine of its thread that wants it. The client connections, the notification senders and the
registration probe pin what they read and wait without a lock.

## 3. The scheduler - corBase

- **`corCo`** - the switch: callee-saved registers and the stack pointer, in assembly, x86-64 and
  aarch64. A yield and a resume take ~23 ns; a coroutine created, run and finished ~29 ns.
- **Stacks**: 256 KiB of address space each (`MAP_NORESERVE` - only the pages a request touches cost
  memory), a guard page below it (an overflow is a SIGSEGV the crash report names), from a pool per
  thread.
- **`corCoLoop`** - waits and timers on the thread's epoll. A waiting coroutine's event pointer is
  tagged (its low bit), so the loop tells it from its own connections: `corCoLoopEvent` for each
  event, `corCoLoopTimeoutMs` for the `epoll_wait` timeout, `corCoLoopExpire` after each batch.
- **`corCoLoopPark` / `corCoLoopWake`** - a coroutine waits until another of its loop wakes it.
- **`corCoBlocking`** - a blocking call on a thread of its own (§ 2).

Both are tested in `corBaseTest`, on x86-64 and on aarch64 (`make arm64-test`, under QEMU).

## 4. The servers

**Built-in HTTP server (corHttp).** One loop per core (`--httpLoops`), on one port: loop 0 accepts
every connection and deals them out in turn (`corHttpAcceptShare`), so connections are spread evenly
- the kernel's own `SO_REUSEPORT` hash splits a handful unevenly. A coroutine's response goes out on
the loop's thread (`corHttpResumeHere`); one that finished without ever waiting is answered when the
server's callback returns.

**cor:// server.** A connection is armed once and multiplexed: the loop reads every frame that comes,
starts each request (inline or as a coroutine), and queues each response as it finishes -
`doc/cor-protocol.md` § 5.3.

**cor:// client.** One connection a peer per thread, shared by the requests of the thread, any number
in flight; `corRestCorStart` / `corRestCorWait` send now and collect later, which is how a distributed
operation has every cor:// forward in flight at once - `doc/cor-protocol.md` § 5.3.

## 5. Numbers

`doc/performance.md`: "Where a request runs" (one core), the three-broker chain, and the write that
notifies; `doc/cor-protocol.md` § 6 for cor://.

## 6. Open questions

- The cap and the stack size as options? (`--coroutines`, `--coStack`)
- libmicrohttpd: its `MHD_suspend/resume_connection` for coroutines too, or the built-in server as
  the default?
- A yielding mongoc stream (`mongoc_client_set_stream_initiator`), so `mongoc` requests can be
  coroutines?
