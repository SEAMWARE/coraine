# Performance and footprint

Every number on this page was measured on one machine, from release builds, and
each section says how. The per-core throughput of the libmicrohttpd builds - the
throughput tables, the writes table and the container section - was re-measured on
2026-09-30; everything else (size, RAM, start-up, the `corHttp` rows, the client
curve, the database and eight-core sections) is from 2026-09-16 and says so. Nothing
here is a vendor estimate or a figure carried over from an earlier version. The
summary table is in the [README](https://github.com/SEAMWARE/coraine#footprint-and-speed);
this is the working underneath it.


## Where these numbers come from

One machine, and a laptop one:

| | |
|---|---|
| CPU | AMD Ryzen 9 8940HX — 16 physical cores / 32 threads, 64 MiB L3, 5.39 GHz boost |
| RAM | 60 GiB |
| OS | Ubuntu 26.04, gcc 15.2.0, x86-64 |
| Build | `CMAKE_BUILD_TYPE=Release` (`-O2`), stripped, `COR_FEATURE_ICU_COLLATION=OFF` |
| Beside it | MongoDB (container, replica set) - 4.4 for the `mongoc` figures on this page, 8.2 since 2026-10-01 · PostgreSQL 18 + TimescaleDB 2.25 (host) |

Four builds are measured throughout — the two axes that change what a coraine
process is made of:

- **HTTP server** — `COR_HTTP_SERVER=builtin`, the epoll server in `corHttp`,
  with no external dependency at all; or `COR_HTTP_SERVER=mhd`, the external
  libmicrohttpd.
- **Current-state DB** — `--database corDB`, entities in this process's own RAM;
  or `--database mongoc`, entities in a MongoDB server.

TRoE is `none` unless a row says otherwise, and the `admin` API plugin (23 KiB)
is not loaded.

### Latency depends on how deep the CPU sleeps

A core with nothing to do drops into an idle state, and the deeper the state the
longer it takes to wake. A latency measurement with one client spends most of its
time waking cores: the three-broker cor:// chain (`doc/cor-protocol.md` § 6.1),
one client, gave **11 000 req/s with the idle states as the machine ships them,
and 14 200 with the deep ones disabled** - the same build, minutes apart.
Throughput with many clients barely moves, since the cores never get to sleep.

So every latency figure states the setting, and two of them are only compared
under the same one. `test/perf/cpuIdle.sh shallow` disables the states deeper
than ~10 µs, `deep` restores them, `show` says which is active (it needs one sudo
rule for `cpupower`, quoted in the script); `corChain.sh` prints the setting on
its first line. Unless a figure says otherwise it was taken **deep** - the
machine's own state, and what a deployment gets.

### Every rate here is per core

A throughput figure with no core count beside it says more about the machine
than about the broker, so the broker is pinned to a stated number of **physical**
cores and the load generator runs on cores the broker was never given. Dividing
a whole-machine total by the core count would be a different measurement — it
assumes the scaling it would be claiming to show.

SMT is the trap, and not a hypothetical: an early run put the load generator on
the **siblings** of the broker's own cores, so the two fought over the same
silicon and the scaling curve turned *down* at the top, which reads exactly like
a broker that fails to scale. The CPU list is read from
`/sys/devices/system/cpu` — one logical CPU per physical core — never assumed.

Each figure is the median of three 5-second `wrk -t8 -c50` runs, and each whole
run waits for the machine to go quiet (load average below 2) before it starts.
Without that wait, a run measured in the wake of the previous one's load
generator came out up to 13% low. Every scenario was measured twice, in
independent passes; the spread between passes was 0.8–7.5%.

The fixture is 100 five-attribute entities of ~550 bytes each, so a `limit=20`
query returns about 11 KB — a realistic page rather than a toy.

A rate measured over error responses is not a rate, so every `wrk` run is
checked for non-2xx answers and the run stops if it finds any. That guard exists
because a create scenario once reported 166 036 creates/s — faster than a
PATCH, impossible — which turned out to be the speed of answering 409 to
duplicate ids.

## Size on disk — what installing coraine adds

Stripped, and counting only what a machine does not already have. The reference
is a bare `ubuntu:26.04`: nine to eleven of the libraries coraine maps — libc,
libm, libstdc++, libgcc_s, libssl, libcrypto, libz, libzstd, libgmp, libresolv,
the dynamic loader, ~14.5 MiB of them — are already in that image for reasons
that have nothing to do with a context broker. They are **mentioned, not
counted**: coraine does not install them and removing coraine does not remove
them.

| Configuration | coraine's own code | Added libraries | **Total added** |
|---------------|-------------------:|----------------:|----------------:|
| `corHttp` + `corDB` | 1.00 MiB | 3.27 MiB (3) | **4.28 MiB** |
| `corHttp` + `mongoc` | 1.08 MiB | 10.04 MiB (6) | **11.12 MiB** |
| libmicrohttpd + `corDB` | 0.99 MiB | 10.56 MiB (12) | **11.55 MiB** |
| libmicrohttpd + `mongoc` | 1.07 MiB | 17.33 MiB (15) | **18.40 MiB** |

**A complete NGSI-LD broker, in-memory store included, is 4.3 MiB of things that
were not on the machine before — and three of those libraries are not ours.**

Those four rows all have TRoE off. Turning history on is the same measure, and
it is the clearest single number on this page:

| Configuration | coraine's own code | Added libraries | **Total added** |
|---------------|-------------------:|----------------:|----------------:|
| `corHttp` + `corDB` + **history in-process** | 1.02 MiB | 3.27 MiB (3) | **4.29 MiB** |
| `corHttp` + `corDB` + history in PostgreSQL | 1.08 MiB | 5.57 MiB (13) | 6.65 MiB |
| libmicrohttpd + `mongoc` + history in PostgreSQL | 1.12 MiB | 19.53 MiB (24) | **20.66 MiB** |

`--troe corDB` — temporal history in this process — adds **no library and no
measurable size**. The full conventional deployment is 24 libraries against 3,
and 20.7 MiB against 4.3.

What the three columns are made of:

- **coraine's own code is all of it.** The cor libraries are static archives,
  whole-archived into the binary, so that ~1 MiB is not a `main` calling out to
  something else — it *is* (libmicrohttpd + `corDB`, 2026-10-01, from the linker
  map) the broker's own service routines (319 KiB), `corNgsild` (303 KiB of
  NGSI-LD rules), the broker's main program (107 KiB), the exported-symbol tables
  that let plugins link against the broker (96 KiB), the bridge layer (49 KiB),
  `corRest` (46 KiB), `corJsonld` (35 KiB), `corArgs` (25 KiB), `corJson`
  (21 KiB), and `corTree`, `corBase`, `corLog`, `corAlloc`, `corProm`,
  `corPlugin`, `corHash` under 9 KiB each - `corHttp` (12 KiB) in its place in the
  builds that use it. Every line of it is in this project's repositories. Add the DB plugin (`corDB.so`
  39 KiB or `mongoc.so` 116 KiB) and `none.so` (14 KiB) and that is the whole
  broker.
- **The three added libraries in the first row** are GEOS (`libgeos` +
  `libgeos_c`, 3.17 MiB — geo-queries) and `libmosquitto` (106 KiB — MQTT
  notifications; since then moved out of the broker into the `mqtt.so` bridge plugin). GEOS is now the largest single thing coraine puts on a machine,
  and it is larger than coraine.
- **libmicrohttpd costs nine libraries, 7.29 MiB** — not its own 608 KiB, because
  it pulls GnuTLS behind it, and p11-kit, nettle, hogweed, tasn1, idn2,
  libunistring, libffi. Building the HTTP server in removes all nine for 12 KiB
  of `corHttp`. That is the 3-vs-12 in the table.
- **`mongoc` costs three libraries, 6.77 MiB** — `libmongoc`, `libbson`,
  `libsasl2` — before MongoDB itself is installed anywhere.

> **ICU is off in every row above, deliberately.** § 7.6.2.1 names ICU "root"
> collation as the default order for `orderBy` on strings, and linking ICU for it
> costs **three libraries and 39.2 MiB** — `libicudata` alone is 31.6 MiB, nine
> times the whole broker, for a collation table. `COR_FEATURE_ICU_COLLATION=ON`
> buys it back; the ICU-free build sorts ASCII exactly as root collation does and
> approximates the rest. Replacing it with a collation implementation of our own
> — the common case first, tailorings added on demand — is on the roadmap.
## RAM

Resident set of the broker process. *Private dirty* is the part no other process
shares: what a second broker on the same machine actually costs.

| Configuration | Idle RSS | Private dirty | 100 000 entities | Per entity |
|---------------|---------:|--------------:|-----------------:|-----------:|
| libmicrohttpd + `corDB` | **13 MiB** | **3 MiB** | 358 MiB | 3.53 KiB |
| `corHttp` + `corDB` | 17 MiB | 9 MiB | 354 MiB | 3.45 KiB |
| libmicrohttpd + `mongoc` | 20 MiB | 4 MiB | 68 MiB *+ mongod* | — |
| `corHttp` + `mongoc` | 24 MiB | 10 MiB | **45 MiB** *+ mongod* | — |

The built-in server starts ~4 MiB heavier, and deliberately: it allocates its
connection pool at start-up, so no request ever calls `malloc` for its own
machinery. libmicrohttpd reserves far more than that, but virtually —
the stacks of its thread pool and of the worker threads put its `VmSize` in the
gigabytes, almost none of it touched.

With **`corDB` an entity is RAM**, and that is the number to size a box with:
**~3.5 KiB resident per five-attribute entity**, so 100 000 entities is ~355 MiB
and a million is ~3.5 GiB. With `mongoc` the entities are MongoDB's problem and
the broker stays nearly flat — but then MongoDB is on the machine, which is the
next two sections.

**glibc's allocator arenas cost 10-25% on top.** With many threads, glibc gives
them separate arenas, and memory freed in one is not handed back to the
operating system. `MALLOC_ARENA_MAX=2` (libmicrohttpd + `corDB`, 2026-10-01):

| Entities | default | `MALLOC_ARENA_MAX=2` |
|---:|---:|---:|
| 10 000 | 89 MiB | 67 MiB |
| 100 000 | 376 MiB | 335 MiB |

Whether the fewer arenas cost throughput with 32 threads has not been measured
yet, so it is not the default; in a container with a tight memory limit it is the
first thing to set. The broker's [memory budget](installation.md#memory-budget)
counts the resident set, arenas included.

> The `mongoc` figures here are lower than this page carried before (68 MiB
> against 202) and the difference is the **measurement**, not the broker. The
> entities used to be loaded one POST at a time; they are now loaded in batches
> of 200, so the broker handles 500 requests instead of 100 000 and carries far
> less request-arena footprint afterwards. Both are real numbers about real
> deployments — the label has to say which.

## What else has to be running

| `--database` / `--troe` | Also required | What that costs |
|-------------------------|---------------|-----------------|
| `corDB` / `none` | **nothing** | — |
| `corDB` / `corDB` | **nothing** | temporal history in the same process, and it is free — see below |
| `mongoc` | a MongoDB server | 1.02–1.15 GiB resident at rest, and WiredTiger's cache defaults to half of (RAM − 1 GiB): on this 60 GiB host mongod is entitled to ~30 GiB. Plus **cores** — see below. Image: `mongo:4.4` 594 MB, `mongo:8` 1.3 GB |
| `timescale` (TRoE) | a PostgreSQL + TimescaleDB server | 35 MB of packages, 128 MiB of shared buffers by default, 313 MiB across its 10 processes here — and on the broker's own machine **2.3 MiB and 9–10 added libraries** for `libpq`, which drags in Kerberos, LDAP and SASL that a broker never calls. Nine rather than ten when `mongoc` has already brought `libsasl2` |

Which is the point of the first two rows. A `corHttp` + `corDB` deployment with
`--troe corDB` is **one process, 17 MiB, 4.3 MiB of new files on disk, and no socket
to anything else** — with temporal history included.

## Start-up

From `exec` to a served HTTP response:

| Configuration | Ready in |
|---------------|---------:|
| libmicrohttpd + `corDB` | **9 ms** |
| `corHttp` + `corDB` | 12 ms |
| libmicrohttpd + `mongoc` | 26 ms |
| `corHttp` + `mongoc` | 31 ms |

corHttp's extra 3 ms is the connection pool it allocates up front; mongoc's
extra 17 ms is the handshake with the database server. All four are fast enough
that the broker is not something you keep warm — it is something you start.
Scale-to-zero, per-test instances, one broker per tenant on a gateway: all of
them stop being awkward at 12 ms and 17 MiB.

## Throughput — per core

**One physical core.** `GET /ngsi-ld/v1/entities?type=Vehicle&limit=N`.

The page size is the whole story, and for months every throughput number
quoted anywhere was the `limit=20` one — read by everybody as requests per
second with the "of 20 entities" left off. Requests/s and entities/s are the
same number only at `limit=1`:

| Response | req/s per core | **entities/s per core** |
|---|---:|---:|
| 1 entity | **74 915** | 74 915 |
| 20 entities | 9 822 | **196 440** |
| 100 entities | 2 082 | **208 200** |

*(libmicrohttpd + `corDB`, 2026-10-01. The per-request cost is fixed, so the
bigger the page the more of it is amortised — and the entities/s column is still
climbing at 100.)*

**All four builds, `limit=20`:**

| Configuration | req/s per core | entities/s per core |
|---------------|---------------:|--------------------:|
| libmicrohttpd + `corDB` | **9 822** | **196 440** |
| `corHttp` + `corDB` | 9 508 | 190 160 |
| libmicrohttpd + `mongoc` | 5 858 | 117 160 |
| `corHttp` + `mongoc` | 4 634 | 92 680 |

*(`corDB` rows 2026-10-01; libmicrohttpd + `mongoc` 2026-09-30, `corHttp` +
`mongoc` 2026-09-16, both against MongoDB 4.4 and not re-measured since.)*

One core of a laptop CPU, going through MongoDB, still serves ~5 900 NGSI-LD
queries a second, delivering 117 000 entities each second.

### The thread hop — and why it is gone where nothing waits

Every request used to make the same detour: read on an HTTP I/O thread, handed to
a worker thread, processed there, handed back to the I/O thread to be sent. The
detour exists so that a request that **waits** - a MongoDB round trip, a
distributed operation, an `@context` download - does not hold up the other
connections of its I/O thread.

For a request that waits on nothing it costs more than the request does. `perf
stat` on one core, a `corDB` retrieve: ~93 000 CPU cycles per request with the
hop, two thread switches each, three quarters of the time in the kernel; ~50 000
without it. So with `corDB` (and TRoE `none` or `corDB`), a request of the
`/ngsi-ld/v1/entities` family that cannot wait runs on the thread that read it -
the rest take the detour as before:

| One core, libmicrohttpd + `corDB` | with the hop | **inline** | |
|---|---:|---:|---:|
| retrieve | 43 568 | **83 785** | x1.92 |
| query, 1 entity | 39 564 | **74 915** | x1.89 |
| query, 20 entities | 7 904 | **9 822** | x1.24 |
| PATCH an attribute | 48 562 | **96 965** | x2.00 |
| create | 36 490 | **62 500** | x1.71 |
| merge | 42 445 | **76 783** | x1.81 |
| delete | 52 546 | **117 398** | x2.23 |
| batches (create, update, delete) | | | take the hop (libmicrohttpd) - a coroutine on the built-in server, below |

On four cores a retrieve goes from 123 281 to 271 497 a second. A request still
takes the hop when it **could** wait: `--distributed` with a registration
anywhere, `?ddsSync` (a bridge service's reply), or an `@context` that is not
cached yet - the Link header's, the default one, or an `application/ld+json`
body's. With `mongoc` nothing runs inline: a MongoDB round trip costs far more
than the hop, and the hop is what keeps it from blocking the I/O thread.
`ngsild_requests_inline_total` / `ngsild_requests_worker_total` show the split;
`--noInline` turns it off.

The built-in server (`corHttp`) had a second hop of the same kind: its event loop
is one thread, so it handed every request's post-response phase - the deferred
notifications above all - to a worker as well. Now a request that left nothing
for that phase finishes on the loop too (every read, and every write in a tenant
with no subscription), and `corHttp` catches up:

| One core, `corHttp` + `corDB` | with both hops | **without** | |
|---|---:|---:|---:|
| retrieve | 34 064 | **79 343** | x2.33 |
| query, 1 entity | 32 622 | **72 608** | x2.23 |
| PATCH an attribute | 35 962 | **95 924** | x2.67 |
| create | 29 903 | **59 901** | x2.00 |
| merge | 33 707 | **78 732** | x2.34 |
| delete | 35 948 | **108 607** | x3.02 |

*(`--httpLoops 1`, 2026-10-01.)* A write that a subscription might match still
finishes on a worker: whether it notifies is only known once it has been matched,
and a notification may need an `@context` the broker hosts itself.

### A request that waits: a coroutine, not a hop

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

**Nothing else moved.** libmicrohttpd before and after - every request shape on
one core, `corDB` and `mongoc`, and the chain - within ±5 % run to run (the clients
under it changed: non-blocking sockets, TLS on the loop, a resolver thread).

### Writes, and what batching is worth

Half of what a context broker does is writes, and until 2026-09-15 this page
had no write number on it at all. That was also the half that was broken:
`corDB` had no locking whatsoever, and twenty concurrent PATCHes killed the
broker every time. A read-only benchmark could never have noticed.

**Per core, `corDB`:**

| Operation | req/s | **entities/s** | vs one at a time |
|---|---:|---:|---:|
| `PATCH` one attribute, 50 clients | 96 965 | 96 965 | — |
| `PATCH`, 1 client | 47 003 | 47 003 | — |
| batch update, 20 per request | 8 056 | **161 120** | **1.7×** |
| merge (`PATCH /entities/{id}`), 50 clients | 76 783 | 76 783 | — |
| create one entity | 62 500 | 62 500 | — |
| batch create, 20 per request | 8 442 | **168 840** | **2.7×** |
| delete one entity | 117 398 | 117 398 | — |
| batch delete, 20 per request | 20 556 | **411 120** | **3.5×** |

*(2026-10-01, libmicrohttpd. Deletes are measured against a store of 40 000
entities, refilled before every repeat - a delete consumes what it measures.)*

Batching is worth two to four times per entity: one HTTP request, one
URL-parameter parse, one `@context` resolution and one lock acquisition amortised
over twenty instead of paid twenty times. It was four to eight times until
single requests stopped making the thread hop (above) - a single write got twice
as fast, a batch did not, because a batch still takes the hop.

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

### Clients piling onto one core

Same core, same query, more clients:

| Clients | libmicrohttpd | p99 | `corHttp` | p99 |
|---:|---:|---:|---:|---:|
| 10 | 6 239 | 3.43 ms | 5 359 | **1.79 ms** |
| 50 | 6 030 | 106.8 ms | 5 834 | **42.9 ms** |
| 200 | 6 147 | 206.0 ms | 4 821 | **75.8 ms** |
| 1000 | 5 654 | 292.0 ms | 4 790 | 303.0 ms |

Throughput is **flat across a hundredfold increase in clients** — 6 239 to
5 654, down 9%. The additional clients wait; they do not make the broker slower
at serving the ones already there. That is the property worth having, and it is
a different claim from a peak.

corHttp holds p99 two to three times lower up to 200 clients. Past that both
servers are queueing and the tail is the queue, not the server.

### Tried, measured, and dropped

Not every idea that should make a broker faster does. Measured on the same
machine, release builds, and left out:

- **Link-time optimisation** (`-flto` across the libraries, 2026-09-30): noise.
  About three quarters of a small request's cycles are spent in the kernel - its
  system calls and thread switches - and LTO can only work on the quarter that is
  ours. It did find a real bug on the way: a `corAlloc()` result used unchecked
  in the JSON parser (fixed). Profile-guided optimisation is still to be measured,
  at the end of the coroutines step (`doc/coroutines.md`).
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

### A regression, and how it was found

On 2026-09-26/27 the nightly performance run went red: creates had fallen 35 % with
`mongoc`. A merge of 09-25 had put a full retrieve of the entity before
**every** `POST /entities` - needed only when a bridge Channel sends the entity
on, done always. The gate only tripped on the second bad night (a single slow
run is noise to it), which is also how close a regression comes to being absorbed
into the median. It was bisected with the images every merge publishes - a broker
per merge, measured in quarters - and fixed by doing the retrieve only when a
Channel is there (coraine#171): `mongoc` creates back to ~9 400/s with 50
clients, confirmed by the next nightly.

## In a container

Every rate above is the broker running **natively**. In a container, the same
build on the same core loses nothing - or up to a third - depending on how the
container is networked:

| One core, 50 clients unless noted | `--net=host` | `-p 1026:1026` (port mapping) |
|---|---:|---:|
| `corDB`, queries and single-entity writes | -0.1% to +11% (no loss) | **+3% to -14%** |
| `corDB`, batch requests | ±2% | -2% to -4% |
| `corDB`, **one client** (create, `PATCH`) | +4% | **-30%** |
| `mongoc`, queries and writes | ±5% | -5% to -17% |
| `mongoc`, batch requests | ±4% | 0% to -5% |

A port mapping puts Docker's address translation (and, depending on the daemon's
configuration, its userland proxy) between the client and the broker: one more
hop per request. Fifty concurrent clients overlap it; a single client waits it out
on every request, which is where the -30% comes from. With `--net=host` there is
no such hop, and nothing else about the container costs measurable throughput.

*(2026-09-30, the same release build as a native binary and as an image, broker on
one physical core via `taskset` / `--cpuset-cpus`, load generator on the others.)*

> ⚠️ Pin both sides when measuring this. Unpinned, the load generator and a
> containerized broker compete for the same CPUs, and the container - in a
> different cgroup - loses that contest: large query responses looked 40% slower in
> a container than natively. With both pinned the difference is gone. `perfRun.sh`
> pins a containerized broker with `--cpuset-cpus {CPUS}` in `PERF_BROKER_CMD`.

## What the database costs

Every figure above pins the *broker* to one core and lets the database have the
rest of the machine. That is the right way to measure a broker core, and it
quietly assumes a database that is never the limit. Here is the price of that
assumption.

**Cores of *deployment* behind one saturated broker core** — measured CPU of
both processes, not assumed:

| | `corDB` | `mongoc` |
|---|---:|---:|
| query, `limit=20` | **1.00** | 1.65 |
| `GET /entities/{id}` | **1.00** | 2.27 |
| `PATCH` | **1.00** | 3.61 |
| batch update | **1.00** | **5.06** |

**And the sweep that answers "how much does MongoDB need in order not to be the
bottleneck?"** — broker fixed at one core, mongod's core budget narrowed with
the container's cpuset:

| mongod cores | query | `PATCH` | batch-20 |
|---:|---:|---:|---:|
| 1 | 4 978 | 4 491 | 544 |
| 2 | 4 862 | 7 776 | 959 |
| 3 | 4 517 | **8 553** | 1 243 |
| 4 | 4 486 | 8 265 | **1 392** |
| 5 | 4 838 | 8 148 | 1 390 |
| 6 | 4 604 | 8 290 | 1 426 |
| 7 | 4 499 | 8 193 | 1 388 |

**Reads need one mongod core. Writes need three, and batch writes four.** Five
cores of machine to do what `corDB` does on one.

### One machine, eight cores, shared by everything

The tables above are per broker core. This one is the question somebody buying
a machine actually asks: eight cores, and whatever the configuration needs
running on them. The load generator is not in the budget — it stands in for
clients, which are somebody else's machines.

| Configuration | `limit=1` | `limit=20` | ent/s | `PATCH` | batch-20 | ent/s |
|---|---:|---:|---:|---:|---:|---:|
| `corDB` | **152 448** | **40 073** | **801 460** | **125 187** | 17 239 | **344 780** |
| `corDB` + history in-process | 150 056 | 40 257 | 805 140 | 123 307 | 17 013 | 340 260 |
| `corDB` + history in PostgreSQL | 147 654 | 39 806 | 796 120 | 11 099 | 1 055 | 21 100 |
| `mongoc` | 60 527 | 23 185 | 463 700 | 19 961 | 1 994 | 39 880 |
| `mongoc` + history in PostgreSQL | 59 754 | 22 947 | 458 940 | 8 251 | 753 | 15 060 |

Three things fall out of that table.

**Temporal history in-process is free.** `--troe corDB` against `--troe none`:
40 257 against 40 073 on queries, 123 307 against 125 187 on PATCH. Within the
noise, on every shape. History in PostgreSQL costs `corDB` **91% of its PATCH
rate and 94% of its batch rate** — not because PostgreSQL is slow, but because
`corDB`'s writes are otherwise nearly free, so the database becomes all of the
cost. On `mongoc`, where writes already cost something, TRoE takes a further
59%.

**Queries do not care.** Every configuration reads at the same speed with
history on or off, which is what you would hope: nothing on the read path
touches the history database.

**Scaling to eight cores is 6.1×**, not 8: 6 588 req/s on one core against
40 073 on eight. The missing 24% is the store's lock and the memory system,
and it is measured rather than extrapolated.

> ⚠️ The `limit=1` column may be partly **load-generator bound**. All three
> `corDB` rows land at 147–152k, and at that rate `wrk` on eight physical cores
> is doing ~19 000 requests/s per core of its own. Treat those as a floor.

> ⚠️ `corDB` **does not persist yet**, so its rows are not like-for-like with
> anything backed by a database server: one of them survives a restart. A
> persisting `corDB` will cost something this page cannot yet quote. It will not
> cost libraries — everything persistence needs is in libc.

## An open question: `--connectionPoolSize`

`corHttp` came out **slower than libmicrohttpd on one core** in the table above
(5 901 against 6 588), and it used to be the other way round. The loop count is
not the cause — `--httpLoops` resolves to 1 on one core, which is correct, and
forcing 2 or 4 there makes it worse, as it should.

The suspect is `--connectionPoolSize`, which defaults to 32. Swept on one core,
built-in server:

| `--connectionPoolSize` | req/s | p99 |
|---:|---:|---:|
| 2 | 6 469 | **9.97 ms** |
| 4 | 6 459 | 20.2 ms |
| 8 | **6 905** | 36.2 ms |
| 16 | 5 082 | 50.8 ms |
| 32 (default) | 5 562 | 44.0 ms |
| 64 | 5 850 | 38.8 ms |

There is something real in the low end — 2 and 8 both beat the default, and 2
holds a tail four times shorter — but **this is not yet a number to act on**,
because 16 measured worse than both 8 and 32 and a genuine trend does not do
that. Either the run-to-run spread is larger than the effect, or something
non-monotonic is going on.

And the option cannot simply be re-defaulted, because it means three different
things at once:

| | libmicrohttpd | `corHttp` |
|---|---|---|
| `corRestBackendStart(poolSize)` | libmicrohttpd's **I/O thread count** | connection **slots**, `poolSize × 16` |
| `corRestWorkerPoolStart(poolSize)` | request **worker threads** | request **worker threads** |

So lowering it to 8 cuts libmicrohttpd's I/O threads fourfold — measured at 15%
of its throughput — while for `corHttp` it lowers workers *and* connection
capacity together, so the sweep above cannot say which of the two the gain came
from. One knob, three concepts, and a default that was sized on a 16-core
laptop.

Separating them is the work: a worker count that is its own option, sized per
CPU the way `--httpLoops` is, and a connection capacity that stays a capacity.
Tracked in [ToDo § 17](https://github.com/SEAMWARE/coraine/blob/main/ToDo.md).

## Reproducing this

### On your own machine, from a published image

Don't take these numbers on trust. [`test/perf/reproduce.sh`](https://github.com/SEAMWARE/coraine/blob/main/test/perf/reproduce.sh)
produces them from nothing but a published image, Docker and `wrk`, and it is
written to be read before it is run: the load scripts are inside it.

```bash
curl -fsSLO https://raw.githubusercontent.com/SEAMWARE/coraine/main/test/perf/reproduce.sh
less reproduce.sh                                   # read it first
bash reproduce.sh quay.io/seamware/coraine:<version>
```

It pins the broker to 1 and then 4 **physical** cores (core 0 and the SMT
siblings left out, the topology read from `/sys`), runs `wrk` on other cores,
and reports the median of three runs per scenario: `POST /entities`, batch create
(20 per request), retrieve by id, and a 20-entity query. The store is corDB, in
memory and without persistence, and the report says so beside the figures.

A figure is printed only if it survives two checks: every response was a 2xx,
and, for the writes, the broker's own entity count afterwards matches what `wrk`
saw acknowledged. A batch answered 207 (some entities refused) passes the first
check and fails the second. The report names the image **by digest**, the CPU
and the kernel, so two people comparing results know they measured the same
thing. Linux only: the broker runs with `--network host`.

`CORES="1 2 4 8"`, `DURATION` and `REPEATS` change the sweep.

### From source

- **Throughput, writes, page sizes** — [`test/perf/perfRun.sh`](https://github.com/SEAMWARE/coraine/blob/main/test/perf/perfRun.sh)
  measures the fixed request shapes and prints one JSON object per run. Set
  `PERF_BROKER_CORES=n` to pin the broker to *n* physical cores and the load
  generator off them; `PERF_ENTITIES` sizes the store; `PERF_BROKER_ARGS` and
  `PERF_BROKER_CMD` say what to start, so a documented number names its flags.
- **Core scaling** — [`test/perf/coreScale.sh`](https://github.com/SEAMWARE/coraine/blob/main/test/perf/coreScale.sh)
  reads the CPU topology rather than assuming it.
- **Forwarding** — [`test/perf/corChain.sh`](https://github.com/SEAMWARE/coraine/blob/main/test/perf/corChain.sh)
  `http|cor|cor-all`: three brokers, a GET on the first forwarded to the third.
- **Latency at a fixed rate** — corTools `corRequest --url http://host:port --path ... -c 16 --rate 20000`:
  each connection on a schedule, latency counted from when a request was due (as
  wrk2 does). Two builds compared at one rate, not each at the rate it reaches.
- **Size** — a stripped release build, plus the transitive `ldd` closure of the
  binary and the loaded plugins, minus everything a bare `ubuntu:26.04` already
  carries.
- **RAM** — `/proc/<pid>/smaps_rollup`, idle and with the store loaded.
- **Start-up** — `exec` to a served HTTP response.
- **What the database costs** — `/proc/<pid>/stat` utime+stime for both
  processes across a fixed run, and `docker update --cpuset-cpus` to narrow
  mongod's core budget.

The two build axes are `-DCOR_HTTP_SERVER=builtin|mhd` and `--database
corDB|mongoc`; the reference build is ICU-free
(`-DCOR_FEATURE_ICU_COLLATION=OFF`, and `COR_WITH_ICU=0` for `corNgsild`).
[Building from source](building.md) has the rest of the switches.
