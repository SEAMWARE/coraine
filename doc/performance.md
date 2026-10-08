# Performance and footprint

Every number on this page was measured on one machine, from release builds, and
each section says how. Re-measured on 2026-10-05, after corDB's log became
memory-mapped (a write's record is in the kernel before the response) and with the
databases on the host network: the eight-core section, what persistence costs, and
the libmicrohttpd rows of the first throughput tables. The rest of the per-core
throughput - the writes table and the container section - is from 2026-09-30 to
10-02, corDB in RAM; everything else (size, RAM, start-up, the `corHttp` rows, the client
curve, the database and eight-core sections) is from 2026-09-16 and says so. Nothing
here is a vendor estimate or a figure carried over from an earlier version. The
summary table is in the [README](https://github.com/SEAMWARE/coraine#footprint-and-speed);
this is the working underneath it. What was tried and dropped, and the regressions
and how they were found: [history](history/performance.md).


## Where these numbers come from

One machine, and a laptop one:

| | |
|---|---|
| CPU | AMD Ryzen 9 8940HX — 16 physical cores / 32 threads, 64 MiB L3, 5.39 GHz boost |
| RAM | 60 GiB |
| OS | Ubuntu 26.04, gcc 15.2.0, x86-64 |
| Build | `CMAKE_BUILD_TYPE=Release` (`-O2`), stripped, `COR_FEATURE_ICU_COLLATION=OFF` |
| Beside it | MongoDB (container) - 8.2 on the host network since 2026-10-05; the older `mongoc` figures (4.4, then 8.2, each section says when) went through a port mapping · PostgreSQL 16 + TimescaleDB (container, host network) for the eight-core section, PostgreSQL 18 + TimescaleDB 2.25 (host) before it |

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
time waking cores: the three-broker cor:// chain (`doc/cor-protocol-details.md` § 6),
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
checked for non-2xx answers and the run stops if it finds any.

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
  `libgeos_c`, 3.17 MiB — geo-queries) and `libmosquitto` (106 KiB — MQTT,
  loaded with the `mqtt.so` bridge plugin only). GEOS is now the largest single thing coraine puts on a machine,
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

> The entities are loaded in batches of 200 - 500 requests rather than 100 000
> single POSTs. Loaded one at a time, the `mongoc` broker carries far more
> request-arena footprint afterwards (202 MiB against 68): both are real numbers
> about real deployments, and the label has to say which.

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

The page size decides the rate. Requests/s and entities/s are the same number
only at `limit=1` - a figure quoted without its page size says little:

| Response | req/s per core | **entities/s per core** |
|---|---:|---:|
| 1 entity | **83 372** | 83 372 |
| 20 entities | 12 703 | **254 060** |
| 100 entities | 2 737 | **273 700** |

*(libmicrohttpd + `corDB`, PGO release, 2026-10-05. The per-request cost is fixed, so the
bigger the page the more of it is amortised — and the entities/s column is still
climbing at 100.)*

**All four builds, `limit=20`:**

| Configuration | req/s per core | entities/s per core |
|---------------|---------------:|--------------------:|
| libmicrohttpd + `corDB` | **12 703** | **254 060** |
| `corHttp` + `corDB` | 9 508 | 190 160 |
| libmicrohttpd + `mongoc` | 7 342 | 146 840 |
| `corHttp` + `mongoc` | 4 634 | 92 680 |

*(libmicrohttpd rows 2026-10-05, PGO release, `mongoc` against MongoDB 8.2 on the host network,
pinned to seven other cores. `corHttp` rows 2026-09-16, without PGO; its `mongoc` row against
MongoDB 4.4 through a port mapping (`docker run -p`), which costs a database round trip - a floor,
not re-measured since.)*

One core of a laptop CPU, going through MongoDB, still serves ~7 300 NGSI-LD
queries a second, delivering 147 000 entities each second.

### Where a request runs

A request that **cannot wait** - the `/ngsi-ld/v1/entities` family on `corDB` (TRoE `none` or
`corDB`), with every `@context` it needs cached, no registration that could make it distributed, no
`?ddsSync` - runs **inline**, on the I/O thread that read it. A request that **can wait** runs as a
**coroutine** of that thread's event loop on the built-in server and the cor:// server
(`doc/coroutines.md`), and on a **worker thread** with libmicrohttpd. With `mongoc` every request
goes to a worker: a MongoDB round trip blocks inside its driver.

The hand-off to a worker costs two thread switches - on one core, more than a cheap request costs:
`perf stat`, a `corDB` retrieve, ~93 000 cycles a request handed off, three quarters of them in the
kernel; ~50 000 inline. `--noInline` hands every request off; one core, libmicrohttpd + `corDB`:

| One core, libmicrohttpd + `corDB` | `--noInline` | **inline (default)** | |
|---|---:|---:|---:|
| retrieve | 43 568 | **83 785** | x1.92 |
| query, 1 entity | 39 564 | **74 915** | x1.89 |
| query, 20 entities | 7 904 | **9 822** | x1.24 |
| PATCH an attribute | 48 562 | **96 965** | x2.00 |
| create | 36 490 | **62 500** | x1.71 |
| merge | 42 445 | **76 783** | x1.81 |
| delete | 52 546 | **117 398** | x2.23 |

*(2026-10-01.)* On four cores a retrieve: 123 281 a second handed off, 271 497 inline.
`ngsild_requests_inline_total` / `ngsild_requests_worker_total` show the split.

The built-in server also finishes a request's **post-response phase** on its loop: inline when the
request left nothing that can wait (every read, a write no subscription matches), as a coroutine
when it did (a notification to send). With `--noInline` both go to a worker:

| One core, `corHttp` + `corDB` | `--noInline` | **default** | |
|---|---:|---:|---:|
| retrieve | 34 064 | **79 343** | x2.33 |
| query, 1 entity | 32 622 | **72 608** | x2.23 |
| PATCH an attribute | 35 962 | **95 924** | x2.67 |
| create | 29 903 | **59 901** | x2.00 |
| merge | 33 707 | **78 732** | x2.34 |
| delete | 35 948 | **108 607** | x3.02 |

*(`--httpLoops 1`, 2026-10-01.)*

### Requests that wait

**A write that notifies** - one broker on one core, `corHttp` + `corDB`, a subscription matching
every PATCH, the receiver (corTestClient, release, `--traceLevels ""`) on other cores: PATCH with 50
connections **45 000** req/s, p99 **1.1 ms**; with one connection **38 800**, p99 **29 µs**; one
notification per write (2026-10-02, release build without PGO).

**Batches** - a batch can always wait, so it is a coroutine on the built-in server (with `corDB` it
never yields): one core, 20 entities a batch - update **11 418** req/s (p99 4.5 ms), create
**8 039**, delete **28 989** (2026-10-02, release build without PGO).

**Forwarding** - three brokers chained, a GET on A forwarded to C through B (`test/perf/corChain.sh`),
each broker on two cores, `corDB`, the built-in server, the release build with PGO (as the Docker
image), deep idle states, two rounds (2026-10-02):

| A -> B -> C | 1 caller req/s | p50 | 16 callers req/s | p50 | p99 |
|---|---:|---:|---:|---:|---:|
| HTTP all the way, small entity | 8 003-8 067 | 128 µs | 42 992-44 418 | 342-349 µs | 0.85-2.23 ms |
| HTTP all the way, 20 attributes | 4 651-5 694 | 156-217 µs | 24 415-25 127 | 631-654 µs | 1.26-1.30 ms |
| HTTP in front, cor:// between, small entity | 12 829-12 998 | 69 µs | 79 991-80 764 | 202-203 µs | 333-346 µs |
| HTTP in front, cor:// between, 20 attributes | 6 578-8 653 | 112-157 µs | 39 703-40 871 | 373-383 µs | 784-810 µs |
| cor:// end to end, small entity | 11 405-11 407 | 91 µs | 91 490-91 945 | 171-172 µs | 235-237 µs |
| cor:// end to end, 20 attributes | 6 880-6 980 | 145-147 µs | 50 988-53 642 | 292-312 µs | 398-402 µs |

**The p99 at 16 callers over HTTP** is the loops' load, not their cost. In this closed-loop
measurement each loop runs at ~90 % of its core, where a burst queues. At a fixed rate (release build
without PGO, corTools
`corRequest --rate`, latency counted from when a request was due) and 16 connections, HTTP all the
way: small entity at 20 000 req/s p50 174-176 µs, p99 0.23-1.85 ms; at 25 000, p99 0.27-0.81 ms;
20 attributes at 15 000, p50 310-377 µs, p99 0.55-0.65 ms. Past ~30 000 (small) and ~20 000 (20
attributes) the p99 goes to 12-24 ms.

### Writes, and what batching is worth

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
over twenty instead of paid twenty times. (libmicrohttpd: a batch is handed to a
worker; on the built-in server it is a coroutine - above.)

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

### Profile-guided: `make pgo`

A release build compiled twice: once instrumented (`-fprofile-generate`), run through the work it is
measured on (`test/perf/pgoTrain.sh`: perfRun's request shapes, a write that notifies, the three-broker
chain in all three modes), then again with the profile (`-fprofile-use`) - the compiler lays out the
hot paths together and the cold ones out of the way, and inlines and unrolls where the profile says
it pays. Every library is in it, not only the broker. Against the same source and flags without the
profile (2026-10-02, one core, `corDB`, built-in server):

| One core | without | **PGO** | |
|---|---:|---:|---:|
| query, 1 entity | 79 657 | **83 452** | +4.8 % |
| query, 20 entities | 11 574 | **12 702** | +9.7 % |
| query, 100 entities | 2 566 | **2 780** | +8.3 % |
| retrieve | 86 546 | **90 200** | +4.2 % |
| PATCH, 1 connection | 44 349 | **48 101** | +8.5 % |
| PATCH, 50 connections | 98 453 | **100 851** | +2.4 % |
| create | 61 965 | **64 417** | +4.0 % |
| merge | 80 039 | **83 557** | +4.4 % |
| delete | 115 624 | **117 100** | +1.3 % |
| batches of 20 (update / create / delete) | | | +3.2 / +5.3 / +1.8 % |

The Docker image and the nightly performance job are built this way.

Every library the broker links is built in the broker's flavour: `make release`, `make pgo` and
`make tune` build all of them as release (the foundation libraries corBase, corLog, corAlloc,
corArgs, corHash, corTree, corJson, corProm and corHttp included), `make di` as debug, and a release
build fails if the broker or a plugin carries code compiled with `COR_T_ON` (traces). The nightly
performance job has linked release foundation libraries since 2026-10-03 (`make pgo`); before that
(`make i`) they were the debug ones `bootstrap.sh` builds, traces compiled in.

User-space instructions per request: retrieve -4.5 %, PATCH -3.5 %, a 20-entity query -6.4 %. The
three-broker chain, 16 callers, cor:// end to end: +4 % (small entity), +10-15 % (20 attributes); one
caller, HTTP in front and cor:// between: +11 % (p50 95 -> 69 µs). Nothing slower.

### Measured and not used

Link-time optimisation (`-flto`) is noise for the broker - three quarters of a small request's cycles
are in the kernel. What else was tried and left out, and why, is in [the history](history/performance.md).

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

**Requests/s:**

| | coraine + MongoDB | coraine + MongoDB + TimescaleDB | coraine, corDB on disk | coraine, corDB on disk + history | coraine, ramDB | Orion-LD + MongoDB | Orion-LD + MongoDB + PostgreSQL |
|---|---:|---:|---:|---:|---:|---:|---:|
| create | 43 811 | 4 680 | 117 299 | 83 325 | 201 907 | 6 508 | 5 009 |
| create, 1 connection | 8 181 | 748 | 28 540 | 22 816 | 31 407 | 4 203 | 1 043 |
| batch create (20) | 12 070 | 391 | 13 864 | 7 955 | 51 141 | 3 610 | 1 804 |
| merge | 27 150 | 7 453 | 101 528 | 80 441 | 124 902 | 6 437 | 5 308 |
| `PATCH` | 17 486 | 8 478 | 145 664 | 120 379 | 174 382 | 6 519 | 5 497 |
| `PATCH`, 1 connection | 3 891 | 888 | 38 332 | 36 714 | 39 631 | 3 504 | 964 |
| batch update (20) | 3 130 | 790 | 34 762 | 23 239 | 40 501 | 2 472 | 2 120 |
| `DELETE` | 28 399 | 9 920 | 205 280 | 183 448 | 221 244 | 18 912 | 9 343 |
| batch delete (20) | 7 351 | 2 134 | 67 070 | 48 329 | 69 480 | 3 163 | 2 467 |
| `GET /entities/{id}` | 55 595 | 53 776 | 481 812 | 493 889 | 469 348 | 31 727 | 31 441 |
| query, `limit=20` | 24 143 | 22 501 | 76 649 | 78 933 | 76 213 | 5 543 | 5 507 |

**Latency, p50 / p95 / p99, ms:**

| | coraine + MongoDB | coraine + MongoDB + TimescaleDB | coraine, corDB on disk | coraine, corDB on disk + history | coraine, ramDB | Orion-LD + MongoDB | Orion-LD + MongoDB + PostgreSQL |
|---|---:|---:|---:|---:|---:|---:|---:|
| create | 1.08 / 1.44 / 2.37 | 8.97 / 22.3 / 34.7 | 0.32 / 8.38 / 15.6 | 0.42 / 16.4 / 35.9 | 0.23 / 0.37 / 0.45 | 6.86 / 14.6 / 19.3 | 9.41 / 18.3 / 25.4 |
| create, 1 connection | 0.11 / 0.18 / 0.20 | 1.32 / 1.44 / 21.5 | 0.03 / 0.04 / 1.02 | 0.04 / 0.06 / 1.32 | 0.03 / 0.03 / 0.04 | 0.23 / 0.28 / 0.34 | 0.92 / 1.13 / 1.21 |
| batch create (20) | 3.60 / 9.55 / 27.3 | 109 / 261 / 357 | 2.35 / 10.1 / 12.5 | 4.09 / 24.8 / 78.6 | 0.83 / 1.92 / 2.85 | 12.4 / 27.9 / 38.3 | 26.2 / 46.6 / 61.8 |
| merge | 1.76 / 2.15 / 2.50 | 6.09 / 11.5 / 13.7 | 0.46 / 0.86 / 1.06 | 0.54 / 1.15 / 1.43 | 0.38 / 0.63 / 0.76 | 7.35 / 11.9 / 15.2 | 8.95 / 17.0 / 22.9 |
| `PATCH` | 2.73 / 3.20 / 3.48 | 5.10 / 11.8 / 15.6 | 0.32 / 0.56 / 0.69 | 0.36 / 0.76 / 0.94 | 0.26 / 0.45 / 0.54 | 7.25 / 11.8 / 14.7 | 8.63 / 16.6 / 22.8 |
| `PATCH`, 1 connection | 0.24 / 0.29 / 0.31 | 1.12 / 1.27 / 1.35 | 0.02 / 0.03 / 0.03 | 0.03 / 0.03 / 0.03 | 0.02 / 0.02 / 0.03 | 0.28 / 0.29 / 0.32 | 1.04 / 1.09 / 1.19 |
| batch update (20) | 15.3 / 17.8 / 19.1 | 58.1 / 112 / 137 | 1.32 / 1.83 / 2.51 | 1.95 / 2.97 / 3.97 | 1.17 / 1.37 / 1.50 | 18.6 / 31.4 / 39.3 | 21.7 / 40.9 / 53.8 |
| `DELETE` | 1.65 / 1.98 / 2.49 | 3.51 / 9.92 / 13.4 | 0.19 / 0.37 / 0.45 | 0.23 / 0.40 / 0.47 | 0.17 / 0.37 / 0.46 | 2.46 / 3.91 / 5.37 | 5.09 / 8.93 / 12.0 |
| batch delete (20) | 5.71 / 13.3 / 15.9 | 16.2 / 47.9 / 65.4 | 0.66 / 0.86 / 0.96 | 0.92 / 1.34 / 1.49 | 0.57 / 0.80 / 1.18 | 13.7 / 27.0 / 35.9 | 16.5 / 38.1 / 75.2 |
| `GET /entities/{id}` | 0.86 / 1.11 / 1.33 | 0.89 / 1.14 / 1.36 | 0.10 / 0.17 / 0.18 | 0.09 / 0.13 / 0.14 | 0.10 / 0.16 / 0.17 | 1.47 / 2.79 / 4.11 | 1.48 / 2.84 / 4.18 |
| query, `limit=20` | 1.98 / 2.76 / 3.36 | 2.12 / 2.99 / 3.60 | 0.63 / 1.16 / 1.17 | 0.54 / 0.93 / 0.94 | 0.60 / 1.06 / 1.08 | 7.94 / 17.2 / 21.7 | 8.01 / 17.5 / 22.4 |

Eight physical cores for the whole deployment - the broker, and the databases it needs (mongod 8.2 and
PostgreSQL 16 + TimescaleDB as containers on the host network - no port mapping - pinned to the same
cores); the load generator on the other eight, standing in for clients. 50 connections unless said,
median of 3 × 5 s, `test/perf/perfRun.sh`, AMD Ryzen 9 8940HX, 2026-10-05. coraine a PGO release,
corDB on disk with `--dbDir`, its log memory-mapped: a write's record is in the kernel before the
response, so a broker that dies loses nothing it acknowledged, and a machine that dies at most the
last 100 ms - MongoDB's default. Orion-LD 1.15.0-next, its release build (`-O3`), `-mongocOnly`.

- **History is where it shows.** In a database server it costs a write 52-97 % (coraine + MongoDB +
  TimescaleDB: create 43 811 → 4 680; Orion-LD + PostgreSQL 14-75 %); in corDB 4-43 %, and coraine with corDB
  and its history outruns MongoDB without any on every write but batch create (20) - and MongoDB + TimescaleDB
  11-41×.
- **Against Orion-LD with its history**: coraine with corDB and its history is 4.4-38× on every row,
  reads included.
- **Persistence costs little:** ramDB, no disk at all, is 1.0-1.7× corDB on disk; 3.7× on batch
  create, which grows the store fastest and so snapshots most.
- **Reads** do not touch history in any configuration. corDB answers a retrieve 8.7× MongoDB's rate,
  481 812 a second at a p99 of 0.18 ms.
- **The tails of a write** follow the throughput: p99 of a batch update 3.97 ms (corDB + history) against
  137 ms (MongoDB + TimescaleDB).

### corDB on disk: what persistence costs

`--dbDir` makes corDB survive a restart: every write appends its effect to a log - memory-mapped, so
the record is in the kernel's page cache before the response - synced every 100 ms (`--dbSync
interval`, the default), with snapshots as the log grows
([corDB's persistence](https://github.com/SEAMWARE/corDB/blob/main/doc/persistence.md)).
`test/perf/perfRun.sh corDB`, PGO release, one tenant, the log on an NVMe disk (ext4), AMD Ryzen 9
8940HX (32 threads, nothing pinned), 2026-10-05 - requests/s, against the same broker without
`--dbDir`:

| scenario | in RAM | `--dbDir` | change | `--dbSync request` | change |
|---|---:|---:|---:|---:|---:|
| query, `limit=20`, c50 | 129 809 | 131 029 | +1 % | 135 354 | +4 % |
| `GET /entities/{id}`, c50 | 612 808 | 617 642 | +1 % | 645 924 | +5 % |
| `PATCH`, c50 | 136 257 | 108 000 | −21 % | 23 478 | −83 % |
| `PATCH`, c1 | 50 511 | 48 467 | −4 % | 1 798 | −96 % |
| merge, c50 | 85 419 | 72 456 | −15 % | 23 903 | −72 % |
| `DELETE`, c50 | 169 207 | 160 588 | −5 % | 20 707 | −88 % |
| batch update (20), c50 | 36 220 | 29 264 | −19 % | 16 920 | −53 % |
| batch delete (20), c50 | 55 676 | 50 342 | −10 % | 12 928 | −77 % |
| create, c50 | 121 491 | 81 895 | −33 % | 22 387 | −82 % |
| create, c1 | 36 925 | 33 204 | −10 % | 1 784 | −95 % |
| batch create (20), c50 | 64 733 | 22 019 | −66 % | 8 716 | −87 % |

- **Reads cost nothing.** A query or a retrieve never touches the log.
- **A write that changes the store costs 4-21 %** - the encoding of its record and its copy into the
  mapped log. A write that updates attributes logs those attributes, not the entity (corDB's design,
  § 3): the more attributes an entity has beside the one a PATCH touches, the more that saves.
- **Creates cost more because the store grows**, and a growing store needs snapshots: batch create
  loses 66 % of its throughput, its p99 23.6 ms against 2.06 ms in RAM - a snapshot takes the write lock
  for a few milliseconds at a time.
- **`--dbSync request` is the disk's speed**: a write answers when its record is synced, so one
  connection does ~1 800 writes/s and fifty share each sync (group commit) for ~21 000-24 000.
- **`--dbCompress` costs the writers that grow the store** (zstd on the snapshot thread): on eight
  cores every scenario within the noise of the same build without it (-2.3 % to +2.9 %), but batch
  create with history, the scenario that snapshots the most: -7.4 %, p99 92 ms against 79. What it
  saves is in "What the data takes on disk".
- **Recovery is not a pause worth planning for**: 100 000 entities (38 MB of log or of snapshot) are
  back in **0.19 s** after `kill -9` (the log replayed) or a clean stop (the snapshot loaded) -
  measured 2026-10-04, the format unchanged since.

What was tried on the way to these numbers, including what did not help, is in
[the history](history/performance.md) and in corDB's
[persistence history](https://github.com/SEAMWARE/corDB/blob/main/doc/history/persistence.md).

### Writes that notify, and which writer the lock lets in first

A deployment that **subscribes** writes far more than it reads: devices report values (`PATCH`) and
every change goes out as a notification. `test/perf/perfRun.sh` measures it as `patch_subs_c50`:
perfRun's 100-entity fixture, **one subscription per entity** (`watchedAttributes: speed`), so each
`PATCH` of `patchAttr.lua` notifies exactly one subscriber - the write, the match and the
notification's HTTP POST all in the number. The receiver is `corTestClient --discard` (counts, keeps
nothing), pinned with the load generator.

Release builds (not PGO), corDB with `--dbDir`, broker pinned to 8 cores of an AMD Ryzen 9 8940HX,
`PATCH` at 50 connections, median of three 5 s runs, three runs per build, 2026-10-07 - requests/s:

| build | `PATCH` | `PATCH`, 100 notifying subscriptions | notifications counted (2 s warm-up + 3 × 5 s) |
|---|---:|---:|---:|
| corDB's store lock as glibc makes it (readers first) | 152 743 | 83 445 | 1.45 M |
| + an attribute's type kept in the node (`CorNode.kind`) | 152 279 | 83 830 | 1.46 M |
| writers first (`PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`) | 158 303 | 80 810 | 1.41 M |
| writers first + the type in the node | 156 026 | 79 893 | 1.39 M |

- **A notifying `PATCH` costs about half the throughput** of a bare one. With a subscription in the
  tenant, the entity is read back after the write and matched, and the notification is sent - an HTTP
  request of its own per `PATCH`.
- **Which writer the store lock lets in first decides nothing alone.** Writers first: a bare `PATCH`
  +2-4 %, a notifying one -3-4 % - the match and the notification read the entity, and a
  writer-preferring lock is slower for readers.

The whole of `perfRun` on the same four builds (the two lock policies two runs each, the default
four) - requests/s, and each against its column's neighbour:

| scenario | readers first | + type in the node | | writers first | + type in the node | |
|---|---:|---:|---:|---:|---:|---:|
| query, `limit=20`, c50 | 71 521 | 72 647 | +1.6 % | 68 940 | 71 660 | +3.9 % |
| query, `limit=20`, c200 | 69 712 | 69 808 | +0.1 % | 66 394 | 68 868 | +3.7 % |
| query, `limit=1`, c50 | 413 490 | 415 226 | +0.4 % | 394 222 | 406 893 | +3.2 % |
| query, `limit=100`, c50 | 15 677 | 15 565 | -0.7 % | 15 470 | 15 388 | -0.5 % |
| `GET /entities/{id}`, c50 | 464 742 | 463 201 | -0.3 % | 438 124 | 457 563 | +4.4 % |
| `PATCH`, c50 | 154 582 | 157 579 | +1.9 % | 154 342 | 157 838 | +2.3 % |
| `PATCH`, c1 | 38 372 | 38 098 | -0.7 % | 38 426 | 38 346 | -0.2 % |
| batch update (20), c50 | 33 542 | 33 974 | +1.3 % | 33 517 | 34 624 | +3.3 % |
| create, c50 | 125 216 | 115 573 | **-7.7 %** | 131 588 | 142 928 | **+8.6 %** |
| create, c1 | 27 731 | 28 007 | +1.0 % | 27 513 | 27 700 | +0.7 % |
| batch create (20), c50 | 15 572 | 16 189 | +4.0 % | 17 894 | 19 028 | +6.3 % |
| merge, c50 | 101 830 | 104 852 | +3.0 % | 109 454 | 116 412 | +6.4 % |
| `DELETE`, c50 | 213 926 | 223 656 | +4.5 % | 204 095 | 207 649 | +1.7 % |
| batch delete (20), c50 | 71 361 | 74 339 | +4.2 % | 69 270 | 72 991 | +5.4 % |

- **Writers first** against readers first (same type form): concurrent creates +5 %, batch creates
  +15 %, merges +7.5 %; queries and retrieves -4-6 %, deletes -4.6 %.
- **Concurrent creates on a durable store, the type in the node: -7.7 %** (readers first) - glibc's
  allocator, not the work. With snapshots off (`--dbSnapshotEvery 65536`) the gap stays (-6.5 %);
  the profile has it in malloc and free (`_int_free_merge_chunk` ×3.8, `_int_free_chunk` ×3): one
  long-lived node less per attribute changes how the short-lived allocations of a request coalesce
  among them. In RAM the two are within 1.5 %; under jemalloc they are equal (181 358 / 181 343), under
  tcmalloc within 2.2 % - and both allocators are faster than glibc's on either form (`create`, c50,
  `--dbDir`, snapshots off: glibc 163 160, jemalloc 181 358, tcmalloc 192 518 - `LD_PRELOAD`, no
  rebuild; the other scenarios not yet measured).
- So the right lock policy is the workload's. corDB keeps readers first by default.

### Which subscriptions a write is matched against

A write is matched only against the subscriptions that **can** match its entity: those that name
its id, those that name one of its types (for a type expression, the first type of each of its OR
groups), and those that select by neither (no `entities`, an `idPattern` alone, type `*`). The
subscription cache keeps them in an index by id and by type (`ldSubCacheCandidates`, corNgsild); the
full match - trigger, entities, watched attributes, scope, q, geoQ - runs on those alone. And an
**Update Attributes** (`PATCH /entities/{id}/attrs`) reads the entity back for a notification only
when a subscription may match it, judged on what the update itself tells: the entity's id, its type
and the attributes it changed (`ldSubscriptionUpdateMayMatch`).

`PATCH` at 50 connections, corDB, release builds, broker on 8 cores of an AMD Ryzen 9 8940HX, the
same day. `make tune`'s runner (`test/perf/tune/tuneRun.sh`): 10 000 Vehicles, 30 s measured after
5 s, median of three - requests/s:

| subscriptions | no index (`main`) | the read-back only on a possible match, no index | **the index** | p99, `main` → index |
|---|---:|---:|---:|---:|
| none | 132 712 | 127 527 | 131 478 (-0.9 %) | 1.41 → 1.45 ms |
| 100, one entity each (1 % of the writes notify) | 119 822 | 118 978 | **125 778 (+5.0 %)** | 1.44 → 1.44 ms |
| 1 000, one entity each (10 % notify) | 91 957 | 104 202 | **121 005 (+31.6 %)** | 1.61 → 1.48 ms |
| 100 entities, a subscription each (every write notifies) | 83 827 | 75 914 | **86 752 (+3.5 %)** | 1.24 → 1.15 ms |

(The middle column was measured on another run of the same evening, its `main` at 127 330 / 115 023 /
84 586 / 79 098: -4.0 % where every write notifies - the subscriptions walked twice, once to decide,
once to match.)

`perfRun.sh` - `patch_manysubs_c50` is new: on top of `patch_subs_c50`'s subscription per entity,
100 subscriptions to 100 other types with `q=temp>25` (the type rejects them) and 10 to `Vehicle`
with `q=speed>200` (the type matches, the q - on the entity - does not). Median of three 5 s runs:

| scenario | corDB `main` | corDB index | | p99 | mongoc `main` | mongoc index | |
|---|---:|---:|---:|---:|---:|---:|---:|
| `PATCH`, no subscriptions | 196 746 | 199 412 | +1.4 % | 0.57 → 0.46 ms | 20 214 | 20 138 | -0.4 % |
| `patch_subs_c50` | 90 124 | 93 571 | **+3.8 %** | 1.38 → **0.83** ms | 16 148 | 16 138 | -0.1 % |
| `patch_manysubs_c50` | 83 234 | 89 084 | **+7.0 %** | 1.39 → **0.89** ms | 15 691 | 15 772 | +0.5 % |

Every other scenario of `perfRun.sh` within -3.5 .. +3.2 % (corDB) and -2.1 .. +1.6 % (mongoc), each
on both sides of zero.

- **What the subscriptions cost is the walk over them, not their number.** Without the index a write
  is matched against every subscription of the tenant; 1 000 of them cost 31 % of the throughput, 110
  more on top of 100 cost 7.6 % (`patch_manysubs_c50` against `patch_subs_c50`, corDB `main`). With
  it, 1 000 subscriptions leave 92 % of the no-subscription rate.
- **Reading the entity back only on a possible match** pays where most writes notify nobody (+23 % at
  1 000, against its own run's `main`) and costs where every write notifies (-4 %) - unless the index makes the deciding cheap.
- **mongoc does not move**: the database round trip is the cost there, not the matching.

### What the data takes on disk

perfRun's fixture entity (five attributes, ~550 bytes of JSON), created with batch creates of 500;
for the history, every entity's `speed` then updated ten times (batch updates). Bytes on disk, the
databases in containers on the host network, 2026-10-05:

**Current state:**

| | 1 000 entities | 100 000 entities | per entity |
|---|---:|---:|---:|
| corDB, `--dbDir` (the log, and the snapshot of a clean stop: the same size) | 740 172 | 74 418 601 | 744 |
| corDB, `--dbDir --dbCompress` (the snapshot of a clean stop) | 12 282 | 1 360 203 | 14 |
| MongoDB 8.2, the documents (`dataSize`, BSON) | 1 067 786 | 107 177 790 | 1 072 |
| MongoDB 8.2, on disk (`storageSize` + `indexSize`) | 196 608 | 11 329 536 | 113 |

**History, ten updates of every entity:**

| | 1 000 entities | 100 000 entities |
|---|---:|---:|
| corDB, `--troe corDB` (`hist-*.cor`) | 4 225 466 | 425 960 175 |
| corDB, `--troe corDB --dbCompress` | 4 225 466 ¹ | 52 081 577 ² |
| TimescaleDB (PostgreSQL 16), `--troe timescale` (the database's growth) | 28 088 840 | 1 727 322 632 |

¹ all of it in the open segment (under 64 MiB), which `--dbCompress` never compresses
² six finished segments compressed, the open one not

- **History: corDB takes a quarter to a sixth** of TimescaleDB's room - with `--dbCompress` a
  thirty-third, once its segments are finished. TimescaleDB's own compression (a policy per
  hypertable) is off, as coraine creates the tables.
- **Current state: without `--dbCompress`, MongoDB takes a sixth of corDB's room; with it, corDB
  takes an eighth of MongoDB's.** corDB's records are a third smaller than MongoDB's documents, but
  MongoDB compresses its files (WiredTiger, snappy) and corDB only with the option. This fixture
  favours a compressor - every entity carries the same 200-character description and the same
  names. And a corDB record decodes on its own, so each one carries its attributes' expanded IRIs
  (`https://uri.etsi.org/ngsi-ld/default-context/speed`) again: most of its 744 bytes.

## An open question: `--connectionPoolSize`

`corHttp` came out **slower than libmicrohttpd on one core** in the table above
(5 901 against 6 588). The loop count is
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
Tracked in [the roadmap, § 17](roadmap.md#17-two-performance-questions-the-2026-09-16-numbers-raised).

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
[Building from source, in detail](building-details.md) has the rest of the switches.
