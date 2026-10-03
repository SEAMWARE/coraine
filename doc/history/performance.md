# Performance - history

What was tried for speed and dropped, regressions and how they were found, and the before-and-after
of the changes behind today's numbers. The numbers as they are now are in
[Performance and footprint](../performance.md). Newest first.

## 2026-09-30 and later - measured, and dropped or found on the way

### Tried, measured, and dropped

Not every idea that should make a broker faster does. Measured on the same
machine, release builds, and left out:

- **corDB persistence: where a create's 25 % goes** (2026-10-03). With `--dbDir` and snapshots off, one
  tenant's creates at 50 connections ran ~25 % below no `--dbDir`. Three suspects measured and cleared:
  *page faults* of the growing log buffer - 0.74 against 0.76 minor faults a request, the same; *PGO
  treating the log as cold code* (pgoTrain runs no `--dbDir`) - a plain `-O2` corDB loses the same
  25 %; *the append under the write lock* - built to return at once, 94-98k against 97k req/s, noise.
  What it is: ~3 µs of CPU a request for the record's encode (39 against 36 µs, the HTTP workers
  CPU-bound) and ~10 % more futex waits a request (2.77 against 2.51, `strace -c`). Moving the encode
  before the write lock (create, replace, batch create and update) took batch create from 19 088 to
  30 692 req/s; single creates did not move - the lock was not their limit.

- **Link-time optimisation** (`-flto` across the libraries, 2026-09-30): noise.
  About three quarters of a small request's cycles are spent in the kernel - its
  system calls and thread switches - and LTO can only work on the quarter that is
  ours. It did find a real bug on the way: a `corAlloc()` result used unchecked
  in the JSON parser (fixed). Profile-guided optimisation, measured after it, is
  not noise - see "Profile-guided" in `doc/performance.md`.
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
  in `doc/history/cor-protocol.md`, "5.3 Multiplexing - on the coroutines".


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
