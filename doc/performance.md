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
counts anonymous memory, the arenas included.

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

Every `perfRun.sh` scenario on `mongoc`, two cores, 2026-10-08: ["mongoc: every scenario"](#mongoc-every-scenario-2026-10-08).

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

### The request arena's sizes

Every request allocates in its own arena: it starts in an inline buffer inside the request state
(`CorRestState.kallocBuffer`, 8 KiB) and grows in chunks (`allocSize`, 256 KiB, the 4th argument of
`corAllocBufferInit` in corRest's `corRestStateInit.c`); everything is freed at once when the request
ends. Swept on 2026-10-08: the chunk at 16, 32, 64, 128, 256 KiB and 1 MiB with the inline buffer at
8 KiB, and the inline buffer at 4, 8, 16 and 32 KiB with the chunk at 256 KiB.

Every configuration was measured the same way, only the two sizes in corRest's source differing:

| | |
|---|---|
| Machine | AMD Ryzen 9 8940HX (16 cores / 32 threads), 60 GiB, Linux 7.0, glibc 2.43, gcc 15.2.0; governor `powersave`, idle states **deep** |
| Build | `make i`: every library and the broker `-O2` release (the libraries under corRest included, rebuilt `BUILD=release`; `corDB.so` from `obj/release`), **no PGO**, libmicrohttpd, coraine `21b18c1c`, corRest `9e2dfac` + the two sizes |
| Store | `--database corDB --dbDir` on the local NVMe disk (ext4), `--dbSync interval` (the default) - the durable configuration; `--troe none` |
| Load | `test/perf/perfRun.sh corDB`, `PERF_BROKER_CORES=2` (the broker on CPUs 0-1, wrk on 2-15), `PERF_DURATION=4s`, `PERF_REPEATS=3` (the median), `PERF_ENTITIES=100` |
| Counters | after each perfRun, a broker started the same way with perfRun's 100-entity fixture: `perf stat -e instructions,cycles -p` over a 4 s wrk run per scenario (c50), divided by the requests wrk counted - whole process, every thread; then a second broker started under `strace -f --seccomp-bpf` for `mmap`, `munmap`, `brk` and `mremap` per request over 2 s windows |
| Noise | the default 256 KiB / 8 KiB measured three times: A before the sweep, B and C after it |

The three default runs differ in one thing: perfRun's two delete scenarios give wrk `-d60s` as a
ceiling and read `Consumed/sec` over the real span, and wrk sleeps the whole ceiling regardless - so
for B, C and every other configuration a `wrk` wrapper on `PATH` cut that one argument to `-d3s`
(the 40 000-entity pool is gone in under a second). A ran with 60 s; its delete rates are within the
spread of B and C.

Requests/s, the change against the mean of A, B and C; for the default, ± half the spread of the
three runs:

| chunk | 16 KiB | 32 KiB | 64 KiB | 128 KiB | **256 KiB** (A / B / C) | 1 MiB |
|---|---:|---:|---:|---:|---:|---:|
| `GET /entities/{id}`, c50 | 161 349 (-4.3 %) | 164 620 (-2.3 %) | 170 187 (+1.0 %) | 166 705 (-1.1 %) | **168 512** (±0.6 %) | 167 426 (-0.6 %) |
| query `limit=1`, c50 | 145 694 (-4.3 %) | 150 904 (-0.9 %) | 151 345 (-0.6 %) | 150 652 (-1.0 %) | **152 238** (±0.2 %) | 152 762 (+0.3 %) |
| query `limit=20`, c50 | 21 786 (-3.1 %) | 21 795 (-3.0 %) | 22 198 (-1.2 %) | 21 772 (-3.1 %) | **22 476** (±1.7 %) | 22 009 (-2.1 %) |
| query `limit=20`, c200 | 21 258 (-3.2 %) | 21 942 (-0.1 %) | 22 152 (+0.9 %) | 22 022 (+0.3 %) | **21 960** (±0.8 %) | 22 035 (+0.3 %) |
| query `limit=100`, c50 | 4 340 (-13.3 %) | 4 578 (-8.5 %) | 5 007 (+0.0 %) | 4 998 (-0.2 %) | **5 006** (±0.3 %) | 5 009 (+0.1 %) |
| `PATCH .../attrs`, c50 | 171 898 (-2.5 %) | 175 669 (-0.3 %) | 176 942 (+0.4 %) | 176 157 (-0.1 %) | **176 274** (±0.8 %) | 177 998 (+1.0 %) |
| `PATCH .../attrs`, c1 | 39 809 (+5.0 %) | 45 362 (+19.7 %) | 45 010 (+18.8 %) | 43 350 (+14.4 %) | **37 899** (±0.3 %) | 44 415 (+17.2 %) |
| merge, c50 | 122 721 (-3.6 %) | 125 831 (-1.1 %) | 127 716 (+0.3 %) | 126 234 (-0.8 %) | **127 275** (±0.9 %) | 130 518 (+2.5 %) |
| batch update (20), c50 | 15 147 (-3.8 %) | 15 521 (-1.4 %) | 15 259 (-3.1 %) | 15 701 (-0.3 %) | **15 745** (±0.6 %) | 15 791 (+0.3 %) |
| create, c50 | 85 573 (-2.5 %) | 89 462 (+1.9 %) | 88 097 (+0.3 %) | 89 027 (+1.4 %) | **87 800** (±1.1 %) | 88 663 (+1.0 %) |
| create, c1 | 27 496 (-2.6 %) | 27 540 (-2.5 %) | 27 525 (-2.5 %) | 27 548 (-2.5 %) | **28 241** (±3.5 %) | 27 736 (-1.8 %) |
| batch create (20), c50 | 9 108 (-9.2 %) | 9 710 (-3.3 %) | 9 830 (-2.1 %) | 9 713 (-3.2 %) | **10 036** (±0.7 %) | 10 122 (+0.9 %) |
| `DELETE`, c50 | 201 699 (-2.9 %) | 210 871 (+1.5 %) | 206 300 (-0.7 %) | 204 922 (-1.3 %) | **207 661** (±3.3 %) | 209 816 (+1.0 %) |
| batch delete (20), c50 | 36 110 (-3.2 %) | 37 943 (+1.7 %) | 38 686 (+3.7 %) | 38 126 (+2.2 %) | **37 311** (±0.8 %) | 36 795 (-1.4 %) |
| `PATCH`, 1 subscriber, c50 | 40 585 (-2.4 %) | 41 210 (-0.9 %) | 40 952 (-1.5 %) | 40 881 (-1.6 %) | **41 564** (±2.0 %) | 40 634 (-2.2 %) |
| `PATCH`, ~210 subscriptions, c50 | 37 878 (-2.2 %) | 38 628 (-0.2 %) | 38 429 (-0.7 %) | 38 292 (-1.1 %) | **38 711** (±2.1 %) | 37 932 (-2.0 %) |

p99 (ms), the same runs:

| chunk | 16 KiB | 32 KiB | 64 KiB | 128 KiB | **256 KiB** (A / B / C) | 1 MiB |
|---|---:|---:|---:|---:|---:|---:|
| `GET /entities/{id}`, c50 | 0.42 | 0.35 | 0.32 | 0.35 | 0.34 / 0.36 / 0.38 | 0.34 |
| query `limit=1`, c50 | 0.61 | 0.36 | 0.38 | 0.39 | 0.61 / 0.40 / 0.43 | 0.36 |
| query `limit=20`, c50 | 2.64 | 2.65 | 2.41 | 3.44 | 2.54 / 3.86 / 2.67 | 2.35 |
| query `limit=20`, c200 | 12.22 | 10.66 | 9.46 | 10.18 | 10.09 / 9.69 / 10.67 | 9.55 |
| query `limit=100`, c50 | 11.76 | 11.49 | 11.49 | 11.80 | 10.79 / 9.75 / 11.17 | 10.06 |
| `PATCH .../attrs`, c50 | 0.55 | 0.73 | 0.97 | 0.80 | 0.41 / 1.81 / 0.72 | 0.66 |
| `PATCH .../attrs`, c1 | 0.03 | 0.04 | 0.03 | 0.04 | 0.04 / 0.03 / 0.03 | 0.03 |
| merge, c50 | 1.93 | 0.89 | 1.01 | 0.94 | 0.88 / 0.81 / 0.51 | 1.12 |
| batch update (20), c50 | 5.98 | 5.11 | 6.04 | 7.11 | 6.21 / 5.50 / 5.66 | 5.48 |
| create, c50 | 5.00 | 4.75 | 4.44 | 4.04 | 4.04 / 3.67 / 4.06 | 4.11 |
| create, c1 | 1.09 | 1.00 | 1.17 | 1.14 | 1.03 / 1.03 / 1.16 | 0.98 |
| batch create (20), c50 | 12.09 | 12.25 | 12.48 | 13.22 | 12.29 / 12.01 / 11.69 | 11.03 |
| `DELETE`, c50 | 0.29 | 0.32 | 0.32 | 0.33 | 0.32 / 0.29 / 0.32 | 0.28 |
| batch delete (20), c50 | 1.87 | 3.87 | 2.15 | 2.58 | 3.41 / 2.02 / 3.08 | 5.61 |
| `PATCH`, 1 subscriber, c50 | 1.77 | 1.54 | 1.59 | 1.58 | 1.61 / 1.81 / 1.54 | 1.53 |
| `PATCH`, ~210 subscriptions, c50 | 2.00 | 1.51 | 1.63 | 1.85 | 1.73 / 1.49 / 1.71 | 1.58 |

Per request (the counters below) and memory:

| chunk | 16 KiB | 32 KiB | 64 KiB | 128 KiB | **256 KiB** (A / B / C) | 1 MiB |
|---|---:|---:|---:|---:|---:|---:|
| retrieve, instructions | 108 628 | 108 866 | 108 492 | 108 566 | 108 767 / 108 501 / 108 976 | 108 574 |
| retrieve, cycles | 57 721 | 57 485 | 57 864 | 57 901 | 57 802 / 57 458 / 58 083 | 57 727 |
| query `limit=20`, instructions | 1 502 188 | 1 500 899 | 1 499 467 | 1 500 744 | 1 500 962 / 1 500 710 / 1 500 002 | 1 500 511 |
| query `limit=20`, cycles | 447 945 | 445 174 | 448 050 | 449 382 | 448 803 / 451 230 / 449 843 | 452 306 |
| query `limit=100`, instructions | 7 553 039 | 7 560 024 | 7 646 750 | 7 639 317 | 7 457 505 / 7 458 261 / 7 688 447 | 7 292 726 |
| query `limit=100`, cycles | 2 376 829 | 2 388 772 | 2 496 427 | 2 502 009 | 2 277 955 / 2 289 715 / 2 548 295 | 2 074 937 |
| `PATCH`, instructions | 81 185 | 81 353 | 80 940 | 81 328 | 81 020 / 81 133 / 81 319 | 81 210 |
| `PATCH`, cycles | 54 691 | 54 468 | 54 182 | 55 162 | 54 894 / 55 157 / 54 733 | 55 729 |
| batch update (20), instructions | 1 456 912 | 1 461 523 | 1 454 823 | 1 471 748 | 1 459 899 / 1 461 565 / 1 461 419 | 1 458 893 |
| batch update (20), cycles | 613 331 | 616 309 | 608 025 | 634 713 | 614 476 / 619 026 / 611 005 | 616 403 |
| RSS after the counters' load, MiB | 64 | 68 | 65 | 38 | 49 / 62 / 56 | 51 |
| peak RSS during perfRun, MiB | 1911 | 2049 | 2064 | 2057 | 2073 / 2122 / 2141 | 2161 |


| inline | 4 KiB | **8 KiB** (A / B / C) | 16 KiB | 32 KiB |
|---|---:|---:|---:|---:|
| `GET /entities/{id}`, c50 | 172 896 (+2.6 %) | **168 512** (±0.6 %) | 171 524 (+1.8 %) | 168 731 (+0.1 %) |
| query `limit=1`, c50 | 154 464 (+1.5 %) | **152 238** (±0.2 %) | 153 346 (+0.7 %) | 153 531 (+0.8 %) |
| query `limit=20`, c50 | 22 152 (-1.4 %) | **22 476** (±1.7 %) | 22 200 (-1.2 %) | 22 129 (-1.5 %) |
| query `limit=20`, c200 | 22 077 (+0.5 %) | **21 960** (±0.8 %) | 21 850 (-0.5 %) | 22 185 (+1.0 %) |
| query `limit=100`, c50 | 4 978 (-0.6 %) | **5 006** (±0.3 %) | 4 984 (-0.4 %) | 5 032 (+0.5 %) |
| `PATCH .../attrs`, c50 | 176 369 (+0.1 %) | **176 274** (±0.8 %) | 176 126 (-0.1 %) | 176 095 (-0.1 %) |
| `PATCH .../attrs`, c1 | 37 854 (-0.1 %) | **37 899** (±0.3 %) | 38 122 (+0.6 %) | 38 202 (+0.8 %) |
| merge, c50 | 131 288 (+3.2 %) | **127 275** (±0.9 %) | 128 971 (+1.3 %) | 128 734 (+1.1 %) |
| batch update (20), c50 | 15 968 (+1.4 %) | **15 745** (±0.6 %) | 15 649 (-0.6 %) | 15 920 (+1.1 %) |
| create, c50 | 90 609 (+3.2 %) | **87 800** (±1.1 %) | 89 407 (+1.8 %) | 88 981 (+1.3 %) |
| create, c1 | 28 212 (-0.1 %) | **28 241** (±3.5 %) | 27 676 (-2.0 %) | 27 595 (-2.3 %) |
| batch create (20), c50 | 10 104 (+0.7 %) | **10 036** (±0.7 %) | 10 210 (+1.7 %) | 9 917 (-1.2 %) |
| `DELETE`, c50 | 205 726 (-0.9 %) | **207 661** (±3.3 %) | 222 069 (+6.9 %) | 194 246 (-6.5 %) |
| batch delete (20), c50 | 37 583 (+0.7 %) | **37 311** (±0.8 %) | 37 829 (+1.4 %) | 36 777 (-1.4 %) |
| `PATCH`, 1 subscriber, c50 | 41 167 (-1.0 %) | **41 564** (±2.0 %) | 41 568 (+0.0 %) | 41 247 (-0.8 %) |
| `PATCH`, ~210 subscriptions, c50 | 38 557 (-0.4 %) | **38 711** (±2.1 %) | 38 808 (+0.3 %) | 38 291 (-1.1 %) |

p99 (ms), the same runs:

| inline | 4 KiB | **8 KiB** (A / B / C) | 16 KiB | 32 KiB |
|---|---:|---:|---:|---:|
| `GET /entities/{id}`, c50 | 0.32 | 0.34 / 0.36 / 0.38 | 0.34 | 0.32 |
| query `limit=1`, c50 | 0.41 | 0.61 / 0.40 / 0.43 | 0.36 | 0.42 |
| query `limit=20`, c50 | 2.70 | 2.54 / 3.86 / 2.67 | 2.52 | 2.31 |
| query `limit=20`, c200 | 9.56 | 10.09 / 9.69 / 10.67 | 9.43 | 9.36 |
| query `limit=100`, c50 | 11.04 | 10.79 / 9.75 / 11.17 | 11.13 | 10.41 |
| `PATCH .../attrs`, c50 | 0.39 | 0.41 / 1.81 / 0.72 | 0.77 | 0.51 |
| `PATCH .../attrs`, c1 | 0.03 | 0.04 / 0.03 / 0.03 | 0.03 | 0.03 |
| merge, c50 | 0.57 | 0.88 / 0.81 / 0.51 | 0.78 | 0.72 |
| batch update (20), c50 | 5.82 | 6.21 / 5.50 / 5.66 | 6.44 | 4.85 |
| create, c50 | 3.96 | 4.04 / 3.67 / 4.06 | 4.20 | 4.58 |
| create, c1 | 1.00 | 1.03 / 1.03 / 1.16 | 1.07 | 1.05 |
| batch create (20), c50 | 13.29 | 12.29 / 12.01 / 11.69 | 12.51 | 11.84 |
| `DELETE`, c50 | 0.29 | 0.32 / 0.29 / 0.32 | 0.30 | 0.32 |
| batch delete (20), c50 | 2.02 | 3.41 / 2.02 / 3.08 | 2.03 | 2.73 |
| `PATCH`, 1 subscriber, c50 | 1.59 | 1.61 / 1.81 / 1.54 | 1.61 | 1.50 |
| `PATCH`, ~210 subscriptions, c50 | 1.64 | 1.73 / 1.49 / 1.71 | 1.60 | 1.53 |

Per request (the counters below) and memory:

| inline | 4 KiB | **8 KiB** (A / B / C) | 16 KiB | 32 KiB |
|---|---:|---:|---:|---:|
| retrieve, instructions | 108 774 | 108 767 / 108 501 / 108 976 | 108 612 | 108 748 |
| retrieve, cycles | 58 417 | 57 802 / 57 458 / 58 083 | 58 476 | 57 791 |
| query `limit=20`, instructions | 1 500 893 | 1 500 962 / 1 500 710 / 1 500 002 | 1 499 687 | 1 500 072 |
| query `limit=20`, cycles | 446 900 | 448 803 / 451 230 / 449 843 | 451 681 | 448 846 |
| query `limit=100`, instructions | 7 667 982 | 7 457 505 / 7 458 261 / 7 688 447 | 7 677 462 | 7 460 278 |
| query `limit=100`, cycles | 2 537 446 | 2 277 955 / 2 289 715 / 2 548 295 | 2 545 081 | 2 277 983 |
| `PATCH`, instructions | 81 594 | 81 020 / 81 133 / 81 319 | 81 079 | 81 177 |
| `PATCH`, cycles | 55 903 | 54 894 / 55 157 / 54 733 | 54 997 | 54 947 |
| batch update (20), instructions | 1 455 258 | 1 459 899 / 1 461 565 / 1 461 419 | 1 452 539 | 1 455 459 |
| batch update (20), cycles | 610 346 | 614 476 / 619 026 / 611 005 | 607 655 | 606 221 |
| RSS after the counters' load, MiB | 42 | 49 / 62 / 56 | 53 | 44 |
| peak RSS during perfRun, MiB | 2111 | 2073 / 2122 / 2141 | 2130 | 2075 |


**What it shows:**

- **A chunk below 64 KiB is slower.** 16 KiB: query `limit=100` −13 %, batch create −9 %, retrieve and
  query `limit=1` −4 %, almost every scenario below the default. 32 KiB: query `limit=100` −8.5 %,
  retrieve −2.3 %. A 100-entity response is ~55 KB, more than one chunk of either.
- **64 KiB to 1 MiB: every c50 scenario within ~3 % of the default.** Query `limit=20` is below the
  default's mean in every other configuration, the three inline sizes at 256 KiB included (−1.2 to
  −1.5 %) - it does not follow the chunk.
- **`PATCH` with one connection is the exception: 37 899 req/s at 256 KiB, 43 350-45 362 at 32, 64,
  128 KiB and 1 MiB (+14 % to +20 %), 39 809 at 16 KiB.** The three default runs gave 37 764, 37 966
  and 37 968 (±0.3 %), and the three inline sizes, all at a 256 KiB chunk, 37 854-38 202. The instructions are the same; the cycles are not. A broker on each size with
  one wrk connection (`wrk -t1 -c1`, `patchAttr.lua`, 4 s, `perf stat -p`):

  | `PATCH .../attrs`, c1, a request | 64 KiB | 256 KiB |
  |---|---:|---:|
  | instructions | 90 773 | 90 316 |
  | cycles | 70 257 | 80 881 |
  | cache misses (`cache-misses`) | 337 | 420 |
  | L1d load misses | 2 471 | 2 508 |
  | dTLB load misses | 4.0 | 3.4 |
  | page faults | 0.006 | 0.008 |
  | `mmap` / `munmap` / `brk` / `madvise` | 0 | 0 |

  Single-connection create does not move with the chunk (27 496-28 241 for every size).
- **The inline buffer: no clear winner.** 4 KiB is up to +3.2 % (retrieve +2.6 %, merge and create
  +3.2 %) - above the default's spread for those three, measured once; 16 and 32 KiB within ~2 % but
  for `DELETE` (+6.9 % at 16 KiB, −6.5 % at 32 KiB), the scenario whose three default runs spread 6.6 %.
- **No size makes the allocator call the kernel.** `mmap`, `munmap`, `brk` and `mremap` per request in
  the steady state: 0.0000 (four decimals) for every configuration and scenario, 1 MiB included -
  glibc's mmap threshold is dynamic: the first freed mmapped block raises it to that block's size,
  and from then on the chunks come from the heap.
- **Memory does not follow the sizes.** RSS after the counters' load is 38-68 MiB for every
  configuration, the three default runs alone 49-62; the peak during perfRun (1.9-2.2 GiB) is the
  store - the 40 000-entity delete pool.
- **The per-request counters of query `limit=100` vary from run to run**: 7.29-7.69 M instructions
  and 2.07-2.55 M cycles across the configurations, 7.46-7.69 M and 2.28-2.55 M across the three runs
  of the default alone - no size is told apart by them. For retrieve, query `limit=20`, `PATCH` and
  batch update the configurations sit within 1-5 % of each other.

**The confirmation** (the same day, 17:25-17:49): 256 KiB and 64 KiB twice each, interleaved, the
conditions above unchanged (coraine `9849dfb6`; the build of run 4 also carried an uncommitted 14-line
change to `haInit.c`, HA start-up, made in the checkout meanwhile). Requests/s, and per request for
`PATCH` with one connection (`wrk -t1 -c1`, 4 s, `perf stat -p` on a broker of its own):

| run | chunk | `PATCH` c1 (perfRun) | `PATCH` c1 (`perf stat` run) | cycles | instructions | cache misses |
|---|---|---:|---:|---:|---:|---:|
| 1 | 256 KiB | 38 210 | 35 230 | 80 962 | 89 931 | 302 |
| 2 | 64 KiB | 39 587 | 35 011 | 81 814 | 90 095 | 319 |
| 3 | 256 KiB | **47 158** | 35 114 | 81 712 | 90 348 | 407 |
| 4 | 64 KiB | 37 563 | **40 903** | 71 680 | 90 005 | 321 |

| run, chunk | retrieve | query `limit=1` | `limit=20` | `limit=20` c200 | `limit=100` | `PATCH` c50 | merge | batch update | create | create c1 | batch create | `DELETE` | batch delete | `PATCH` 1 sub | `PATCH` ~210 subs |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1, 256 KiB | 161 620 | 146 583 | 21 488 | 21 398 | 4 912 | 171 879 | 124 263 | 15 348 | 87 367 | 32 721 | 9 757 | 185 118 | 35 972 | 38 864 | 37 092 |
| 2, 64 KiB | 154 434 | 140 365 | 21 338 | 20 799 | 4 895 | 170 489 | 127 258 | 14 899 | 87 861 | 27 737 | 9 751 | 203 492 | 34 839 | 40 000 | 36 569 |
| 3, 256 KiB | 158 022 | 139 949 | 21 558 | 21 441 | 4 730 | 166 078 | 125 331 | 15 494 | 86 784 | 27 415 | 9 826 | 196 201 | 36 363 | 38 848 | 36 116 |
| 4, 64 KiB | 162 468 | 146 956 | 21 322 | 21 529 | 4 939 | 171 656 | 125 571 | 15 068 | 86 559 | 27 712 | 9 547 | 215 166 | 35 448 | 40 396 | 37 938 |

- **Single-connection `PATCH` has two levels, ~38 000 and ~44 000-47 000 req/s (~81 000 and ~71 000
  cycles a request, the instructions the same), and the chunk does not pick the level**: 256 KiB
  reached the high one (run 3, 47 158), 64 KiB stayed on the low one in perfRun twice and reached the
  high one in one `perf stat` run of four. The sweep's +14-20 % at 32, 64, 128 KiB and 1 MiB was that
  level, not the chunk. Single-connection create shows the same: 32 721 in run 1, ~27 500 otherwise.
- **Every other scenario: 64 KiB against 256 KiB within −2.8 % to +3.5 % on the means of two runs**, the
  same order as the run-to-run spread of each size (`DELETE` +9.8 % against a spread of ~6 %). Every
  rate here is a few percent below the sweep's: a different day's state of the machine; the runs are
  compared only with each other.

**Conclusion:** no size wins beyond the noise. 16 and 32 KiB chunks lose (query `limit=100` −13 % and
−8.5 %); 64 KiB to 1 MiB and every inline size from 4 to 32 KiB are equal within it. **The defaults
stay: chunk 256 KiB, inline 8 KiB.** The single-connection write's two levels are a property of the
process or the machine, not of the arena, and are open.

### What the response byte budget costs

`--maxResponseSize` (0 = none; without it 1/16 of the memory budget, [Response size](installation.md#response-size)) makes
an entity query count the size of each entity it fetches: corDB walks the stored entity
(`corJsonFastRenderSize`), mongoc takes the BSON length. Measured 2026-10-08 with one release binary,
only the flag differing: **A** `--maxResponseSize 0` (no budget), **B** `--maxResponseSize 32` (32 MiB:
the budget of a pod of about 600 MiB), interleaved A, B, A, B.

| | |
|---|---|
| Build | `make i` in coraine `f8b27598` (`feat/response-byte-budget`) with corNgsild `8b0f6b5` and corDB `1c48987` (`feat/query-byte-budget`), corRest `f5164d4`; every library release (`-O2`, no `COR_T_ON` marker in the broker or its plugins), no PGO, libmicrohttpd |
| Machine | AMD Ryzen 9 8940HX, governor `powersave`, idle states deep |
| Load | `test/perf/perfRun.sh`, `PERF_BROKER_CORES=2` (broker on CPUs 0-1, wrk on 2-15), `PERF_DURATION=4s`, `PERF_REPEATS=3`, the flag through `PERF_BROKER_ARGS`; the delete scenarios' wrk ceiling cut from 60 s to 3 s as in "The request arena's sizes" |
| corDB | `--dbDir` on the local NVMe disk (ext4), `--dbSync interval` - the durable configuration |
| mongoc | MongoDB 8.2.12 in a container (`mongo:8.2`) through a port mapping (`-p 27017:27017`), not pinned; one pair A, B |
| Counters | after each corDB run, a broker of its own with the same flag: `perf stat -e instructions,cycles -p` per request, wrk c50, 4 s per scenario |

**corDB**, requests/s; A-A and B-B are the run-to-run spread of the same flag:

| scenario | A1 | B1 | A2 | B2 | A-A | B-B | B vs A | p99 A1 / B1 / A2 / B2 (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| query `limit=1`, c50 | 148 831 | 149 434 | 153 935 | 152 897 | 3.4 % | 2.3 % | -0.1 % | 0.38 / 0.41 / 0.40 / 0.67 |
| query `limit=20`, c50 | 22 351 | 21 874 | 22 056 | 22 582 | 1.3 % | 3.2 % | +0.1 % | 2.31 / 2.55 / 2.44 / 2.24 |
| query `limit=20`, c200 | 22 068 | 21 925 | 22 199 | 22 308 | 0.6 % | 1.7 % | -0.1 % | 9.86 / 9.60 / 9.42 / 9.89 |
| query `limit=100`, c50 | 4 993 | 4 954 | 4 622 | 5 039 | 7.7 % | 1.7 % | +3.9 % | 11.22 / 11.55 / 13.80 / 10.46 |
| `GET /entities/{id}`, c50 | 165 729 | 166 874 | 167 418 | 169 679 | 1.0 % | 1.7 % | +1.0 % | 0.36 / 0.35 / 0.40 / 0.32 |
| `PATCH .../attrs`, c50 | 173 487 | 170 906 | 179 849 | 177 305 | 3.6 % | 3.7 % | -1.5 % | 0.69 / 0.46 / 0.54 / 0.68 |
| `PATCH .../attrs`, c1 | 38 148 | 37 791 | 43 335 | 38 043 | 12.7 % | 0.7 % | -6.9 % | 0.03 / 0.03 / 0.03 / 0.03 |
| merge, c50 | 114 692 | 131 341 | 129 762 | 127 479 | 12.3 % | 3.0 % | +5.9 % | 3.98 / 0.76 / 1.00 / 0.54 |
| batch update (20), c50 | 15 693 | 15 606 | 15 569 | 15 828 | 0.8 % | 1.4 % | +0.6 % | 5.38 / 5.25 / 6.75 / 5.29 |
| create, c50 | 87 277 | 88 265 | 89 794 | 89 538 | 2.8 % | 1.4 % | +0.4 % | 4.21 / 4.73 / 4.54 / 3.82 |
| create, c1 | 33 045 | 27 831 | 32 260 | 32 467 | 2.4 % | 15.4 % | -7.7 % | 0.97 / 0.99 / 0.99 / 0.96 |
| batch create (20), c50 | 9 790 | 9 842 | 10 066 | 10 190 | 2.8 % | 3.5 % | +0.9 % | 13.69 / 12.16 / 13.32 / 12.68 |
| `DELETE`, c50 | 201 881 | 204 728 | 203 394 | 206 958 | 0.7 % | 1.1 % | +1.6 % | 0.34 / 0.30 / 0.32 / 0.28 |
| batch delete (20), c50 | 37 142 | 38 551 | 37 762 | 37 506 | 1.7 % | 2.7 % | +1.5 % | 3.29 / 3.21 / 1.87 / 2.00 |
| `PATCH`, 1 subscriber, c50 | 40 973 | 41 088 | 41 184 | 41 047 | 0.5 % | 0.1 % | -0.0 % | 1.56 / 1.58 / 1.52 / 1.56 |
| `PATCH`, ~210 subscriptions, c50 | 38 448 | 37 677 | 38 582 | 38 494 | 0.3 % | 2.1 % | -1.1 % | 1.85 / 1.64 / 1.70 / 1.71 |

Per request (whole process):

| per request | A1 | B1 | A2 | B2 |
|---|---:|---:|---:|---:|
| query `limit=20`, instructions | 1 481 287 | 1 482 443 | 1 481 469 | 1 481 100 |
| query `limit=20`, cycles | 444 631 | 446 082 | 449 220 | 445 329 |
| query `limit=100`, instructions | 7 198 444 | 7 363 566 | 7 586 171 | 7 585 977 |
| query `limit=100`, cycles | 2 055 124 | 2 242 233 | 2 518 834 | 2 502 515 |
| retrieve, instructions | 107 650 | 107 633 | 107 762 | 107 450 |
| retrieve, cycles | 57 759 | 58 290 | 58 129 | 57 638 |
| `PATCH`, instructions | 80 983 | 81 005 | 81 019 | 80 860 |
| `PATCH`, cycles | 54 355 | 55 311 | 56 651 | 55 396 |

**mongoc**, requests/s, one run each:

| scenario | A | B | B vs A | p99 A / B (ms) |
|---|---:|---:|---:|---:|
| query `limit=1`, c50 | 30 375 | 29 566 | -2.7 % | 3.56 / 3.71 |
| query `limit=20`, c50 | 11 440 | 11 574 | +1.2 % | 7.46 / 6.72 |
| query `limit=20`, c200 | 11 643 | 11 289 | -3.0 % | 20.19 / 20.97 |
| query `limit=100`, c50 | 3 212 | 3 231 | +0.6 % | 28.46 / 28.50 |
| `GET /entities/{id}`, c50 | 36 969 | 36 356 | -1.7 % | 3.31 / 3.24 |
| `PATCH .../attrs`, c50 | 18 505 | 18 261 | -1.3 % | 5.04 / 3.90 |
| `PATCH .../attrs`, c1 | 3 421 | 3 350 | -2.1 % | 0.37 / 1.38 |
| merge, c50 | 22 858 | 23 282 | +1.9 % | 4.66 / 3.43 |
| batch update (20), c50 | 1 675 | 1 555 | -7.2 % | 33.69 / 36.71 |
| create, c50 | 32 709 | 33 838 | +3.5 % | 4.39 / 3.64 |
| create, c1 | 7 467 | 7 213 | -3.4 % | 0.62 / 0.18 |
| batch create (20), c50 | 7 657 | 7 185 | -6.2 % | 25.63 / 23.79 |
| `DELETE`, c50 | 26 122 | 26 323 | +0.8 % | 4.86 / 5.04 |
| batch delete (20), c50 | 3 327 | 3 387 | +1.8 % | 23.47 / 23.78 |
| `PATCH`, 1 subscriber, c50 | 11 723 | 12 076 | +3.0 % | 7.99 / 8.75 |
| `PATCH`, ~210 subscriptions, c50 | 11 264 | 11 745 | +4.3 % | 7.88 / 7.51 |

- **corDB, the queries: B against A −0.1 % (`limit=1`), +0.1 % (`limit=20`), −0.1 % (`limit=20`, c200),
  +3.9 % (`limit=100`)** - each inside its own A-A or B-B spread (up to 7.7 %, `limit=100`). Retrieve +1.0 %.
- **The per-request counters do not separate A from B**: query `limit=20` 1.481-1.482 M instructions in
  all four runs; query `limit=100` 7.20 M (A1), 7.36 M (B1), 7.59 M (A2), 7.59 M (B2) - the second A
  and the second B identical, the spread between runs of one flag larger than between the flags.
- **The writes do not use the budget** and move within their spread; single-connection `PATCH` and create
  show the two levels described in "The request arena's sizes" (38 000 / 43 000 and 28 000 / 33 000).
- **mongoc: queries and retrieve within −3.0 % to +1.2 %**, one pair, no spread of its own; the largest
  moves are writes (batch update −7.2 %, batch create −6.2 %), which do not use the budget.

**Conclusion:** the budget's cost is not measurable - within the run-to-run noise on corDB and on
mongoc, for every query page size. It stays on by default.

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

### The default order and its index on mongoc

An entity query without `orderBy` returns its entities in creation order - `{createdAt: 1, _id: 1}` in
mongoc, backed by the index `{createdAt, _id}`. Until 2026-10-08 a query by type had an index on
`{type}` alone beside it; now it has `{type, createdAt, _id}`, and the broker drops the old `type_1`
from an existing database at start.

What MongoDB reads for one page of `type=X` in that order - `explain("executionStats")`, MongoDB 8.2.12,
a scratch collection of 100 000 documents (~450 bytes) with types at 10 %, 1 % and 0.1 % of them:

| type's share | page | `{type}` - documents read | plan | `{type, createdAt, _id}` - documents read (keys) |
|---|---|---:|---|---:|
| 10 % | `limit=20` | 191 | the `{createdAt,_id}` index, filtered by type | 20 (20) |
| 10 % | `limit=100` | 991 | the same | 100 (100) |
| 10 % | `limit=20`, skip 50 | 691 | the same | 20 (70) |
| 1 % | any of the three | 1 000 | every entity of the type through `{type}`, sorted in memory | 20 / 100 / 20 (20 / 100 / 70) |
| 0.1 % | any of the three | 100 | the same | 20 / 100 / 20 (20 / 100 / 70) |

With `{type}` alone a page reads either (page / the type's share) documents or every entity of the type
plus an in-memory sort - whichever the planner picks; a type of millions of entities on the second plan
meets MongoDB's in-memory sort limit. With the type first in the order's index a page reads the page.

**Measured** with `test/perf/perfRun.sh mongoc`, which has a scenario for it since this change: the store
emptied, 100 000 of perfRun's ~550-byte entities created, one in a hundred of type `Rare` (1 000, spread
evenly through creation order), the rest `Common`, plus the 100-entity fixture - then `type=Rare` with
`limit=20`, `limit=100` and `limit=20&offset=500`, 50 connections. explain() on that store after each
run: A read 1 000 documents (`type_1` and an in-memory sort) for each of the three; B read 20, 100 and
20 (520 keys for the offset) through `type_1_createdAt_1__id_1`.

| | |
|---|---|
| Builds | A = `main` `04785a2c`, B = `perf/mongoc-type-created-index` (the same plus this change); both `make release`, every library release (no `COR_T_ON` marker in the broker or `mongoc.so`), no PGO, libmicrohttpd |
| Machine | AMD Ryzen 9 8940HX, governor `powersave`, idle states deep; MongoDB 8.2.12 in a container (`mongo:8.2`) through a port mapping (`-p 27017:27017`), not pinned |
| Load | `PERF_BROKER_CORES=2` (broker on CPUs 0-1, wrk on 2-15), `PERF_DURATION=4s`, `PERF_REPEATS=3`; the delete scenarios' wrk ceiling cut from 60 s to 3 s as in "The request arena's sizes"; interleaved A, B, A, B, 2026-10-08 22:18-22:43 |

Requests/s; A-A and B-B are the run-to-run spread of the same build:

| scenario | A1 | B1 | A2 | B2 | A-A | B-B | B vs A | p99 ms, A1 / B1 / A2 / B2 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Rare, `limit=20` | 558 | 11 126 | 589 | 11 422 | 5.4 % | 2.6 % | **×19.7** | 163.06 / 7.20 / 167.05 / 6.94 |
| Rare, `limit=100` | 506 | 3 122 | 517 | 3 005 | 2.2 % | 3.8 % | **×6.0** | 125.08 / 29.06 / 131.95 / 38.52 |
| Rare, `limit=20&offset=500` | 513 | 3 697 | 526 | 3 861 | 2.5 % | 4.3 % | **×7.3** | 119.77 / 20.41 / 117.61 / 20.06 |
| query `limit=1`, c50 | 28 239 | 27 454 | 29 107 | 27 044 | 3.0 % | 1.5 % | -5.0 % | 3.93 / 3.82 / 3.72 / 3.82 |
| query `limit=20`, c50 | 11 348 | 10 885 | 11 399 | 11 364 | 0.4 % | 4.3 % | -2.2 % | 7.00 / 7.29 / 7.21 / 7.04 |
| query `limit=20`, c200 | 10 888 | 10 941 | 11 158 | 11 269 | 2.4 % | 3.0 % | +0.7 % | 21.66 / 22.25 / 23.09 / 20.80 |
| query `limit=100`, c50 | 3 067 | 3 137 | 3 133 | 3 092 | 2.1 % | 1.4 % | +0.5 % | 30.47 / 30.68 / 30.15 / 31.29 |
| `GET /entities/{id}`, c50 | 33 677 | 33 608 | 35 180 | 34 325 | 4.4 % | 2.1 % | -1.3 % | 3.20 / 3.52 / 3.40 / 3.69 |
| `PATCH .../attrs`, c50 | 16 771 | 17 395 | 18 004 | 17 316 | 7.1 % | 0.5 % | -0.2 % | 4.31 / 4.29 / 3.96 / 4.11 |
| `PATCH .../attrs`, c1 | 3 217 | 3 287 | 3 358 | 3 242 | 4.3 % | 1.4 % | -0.7 % | 0.43 / 0.98 / 0.40 / 2.67 |
| merge, c50 | 21 813 | 21 909 | 22 862 | 23 138 | 4.7 % | 5.5 % | +0.8 % | 3.60 / 4.52 / 3.85 / 3.47 |
| batch update (20), c50 | 1 545 | 1 634 | 1 648 | 1 618 | 6.5 % | 1.0 % | +1.8 % | 37.03 / 35.34 / 34.44 / 34.76 |
| create, c50 | 31 105 | 31 603 | 32 300 | 32 552 | 3.8 % | 3.0 % | +1.2 % | 4.27 / 4.08 / 4.83 / 3.95 |
| create, c1 | 6 886 | 7 029 | 7 177 | 7 321 | 4.1 % | 4.1 % | +2.0 % | 1.51 / 1.05 / 1.79 / 0.75 |
| batch create (20), c50 | 7 176 | 6 822 | 6 967 | 7 295 | 3.0 % | 6.7 % | -0.2 % | 41.46 / 30.48 / 17.21 / 31.33 |
| `DELETE`, c50 | 24 088 | 23 188 | 25 808 | 26 241 | 6.9 % | 12.4 % | -0.9 % | 6.15 / 6.59 / 4.81 / 4.26 |
| batch delete (20), c50 | 3 245 | 3 210 | 3 347 | 3 150 | 3.1 % | 1.9 % | -3.5 % | 25.52 / 23.96 / 23.84 / 25.11 |
| `PATCH`, 1 subscriber, c50 | 10 684 | 11 703 | 11 165 | 11 810 | 4.4 % | 0.9 % | +7.6 % | 8.80 / 7.74 / 8.73 / 8.03 |
| `PATCH`, ~210 subscriptions, c50 | 10 588 | 11 299 | 11 064 | 11 308 | 4.4 % | 0.1 % | +4.4 % | 9.22 / 8.60 / 8.31 / 8.51 |

- **A rare type's page: 19.7x the requests/s for `limit=20`** (573 -> 11 274), 6.0x for `limit=100`, 7.3x
  for `offset=500`; p99 from 163-167 ms to 7 ms for `limit=20`.
- **The writes do not pay for the wider index entry**: create, batch create, merge, `PATCH` within -0.2 %
  to +2.0 %, inside their spreads (3-7 %); the old `{type}` index is gone, so a write still updates the
  same number of indexes.
- **The other queries** (perfRun's fixture, one type): `limit=20`, `limit=20` c200, `limit=100` within
  -2.2 % to +0.7 %; `limit=1` -5.0 % against spreads of 3.0 % (A) and 1.5 % (B).

**Conclusion:** the index is the change - a query by type reads only the page it returns, whatever the
type's share of the store, at no measurable cost to writes.

**In a larger store**, measured again on `main` after the merge (2026-10-08 23:21 - 2026-10-09 00:08).
`perfRun.sh` has a second rare-type scenario since: the store emptied, 300 000 entities, one in a hundred
of type R1 (3 000, 1 %) and one in a thousand of type R01 (300, 0.1 %), the rest `Common` - a first page
and a deep page of each. explain() on that store after each run:

| `type=`, page of 20 | before (`{type}`): documents read, plan | `main`: documents read (keys) |
|---|---|---:|
| R1 (1 %), first page | 2 100, the `{createdAt,_id}` index filtered by type | 20 (20) |
| R1 (1 %), `offset=1000` | 3 000, every R1 through `type_1`, sorted in memory | 20 (1 020) |
| R01 (0.1 %), first page | 300, every R01 through `type_1`, sorted in memory | 20 (20) |
| R01 (0.1 %), `offset=200` | 300, the same | 20 (220) |

The plan for a 1 % type is the order-index walk in this store and the whole-type sort in the 100 000-entity
one: the planner picks by trial, and the store's size moves the trial's outcome.

Before = `65be8c03` (the commit before the merge of this change, #292), `main` = `6493d6c2`; both `make
release`, every library release, no `COR_T_ON` marker; the conditions of the table above (two broker
cores, 4 s x 3, the delete cap) and the same `perfRun.sh` (`main`'s) for both. Run in the order main,
before, main, before, main. Requests/s:

| scenario | before #292, runs 1 / 2 | `main`, runs 1 / 2 / 3 | change of the means | p99 ms, before / main (means) |
|---|---:|---:|---:|---:|
| 100 000 entities, 1 % type: `limit=20` | 636 / 592 | 11 377 / 11 369 / 11 499 | **×18.6** | 150.15 / 6.98 |
| 100 000, 1 %: `limit=100` | 526 / 523 | 3 157 / 3 177 / 3 200 | **×6.1** | 126.66 / 30.40 |
| 100 000, 1 %: `limit=20&offset=500` | 526 / 526 | 3 653 / 3 928 / 3 910 | **×7.3** | 125.48 / 20.12 |
| 300 000 entities, 1 % type: `limit=20` | 559 / 564 | 11 383 / 11 495 / 11 426 | **×20.4** | 111.03 / 7.25 |
| 300 000, 1 %: `limit=20&offset=1000` | 170 / 171 | 1 953 / 1 960 / 1 750 | **×11.1** | 460.51 / 35.41 |
| 300 000, 0.1 % type: `limit=20` | 1 685 / 1 686 | 11 219 / 11 491 / 11 265 | **×6.7** | 38.40 / 7.00 |
| 300 000, 0.1 %: `limit=20&offset=200` | 1 653 / 1 653 | 6 649 / 7 022 / 6 801 | **×4.1** | 39.20 / 10.97 |

- **Every rare-type page is 4-20x faster**: a first page of 20 is ~11 400 requests/s whatever the type's
  share or the store's size (1 % or 0.1 %, 100 000 or 300 000 entities); before, 559-1 686.
- **The deep page of the 1 % type in the larger store: 170 -> 1 956 requests/s (11x), p99 461 -> 35 ms.**
  `main` still reads the keys before the page (1 020 for `offset=1000`) but not the documents.

The rest of perfRun in the same session:

| scenario | before #292, runs 1 / 2 | `main`, runs 1 / 2 / 3 | change of the means | p99 ms, before / main (means) |
|---|---:|---:|---:|---:|
| query `limit=1`, c50 | 29 423 / 29 272 | 27 106 / 28 446 / 29 033 | -3.9 % | 3.74 / 3.51 |
| query `limit=20`, c50 | 11 707 / 11 720 | 11 264 / 11 416 / 11 438 | -2.9 % | 6.72 / 7.12 |
| query `limit=20`, c200 | 11 225 / 11 610 | 10 822 / 11 353 / 11 405 | -2.0 % | 21.10 / 21.34 |
| query `limit=100`, c50 | 3 135 / 3 176 | 3 088 / 3 193 / 3 230 | +0.5 % | 28.93 / 32.93 |
| `GET /entities/{id}`, c50 | 36 188 / 34 880 | 33 741 / 35 531 / 35 721 | -1.5 % | 3.26 / 3.36 |
| `PATCH .../attrs`, c50 | 17 896 / 18 274 | 17 266 / 18 335 / 18 199 | -0.8 % | 3.94 / 3.95 |
| `PATCH .../attrs`, c1 | 3 192 / 3 348 | 3 178 / 3 182 / 3 468 | +0.2 % | 0.91 / 1.23 |
| merge, c50 | 23 167 / 23 264 | 21 426 / 22 665 / 22 987 | -3.7 % | 3.72 / 3.79 |
| batch update (20), c50 | 1 728 / 1 713 | 1 551 / 1 649 / 1 707 | -4.9 % | 32.60 / 37.12 |
| create, c50 | 33 649 / 33 750 | 30 825 / 33 287 / 33 651 | -3.3 % | 3.88 / 3.98 |
| create, c1 | 7 089 / 7 107 | 6 808 / 7 410 / 7 410 | +1.6 % | 0.78 / 1.04 |
| batch create (20), c50 | 7 257 / 7 063 | 6 005 / 6 222 / 6 682 | -12.0 % | 32.01 / 40.14 |
| `DELETE`, c50 | 25 809 / 26 003 | 23 421 / 25 563 / 26 005 | -3.5 % | 5.37 / 5.22 |
| batch delete (20), c50 | 3 447 / 3 428 | 3 091 / 3 165 / 3 251 | -7.8 % | 23.94 / 25.44 |
| `PATCH`, 1 subscriber, c50 | 11 725 / 11 652 | 11 051 / 11 721 / 11 873 | -1.2 % | 8.03 / 8.48 |
| `PATCH`, ~210 subscriptions, c50 | 11 228 / 11 202 | 10 871 / 11 277 / 11 357 | -0.4 % | 8.43 / 8.30 |

- **The batch writes are lower on `main` in this session**: batch create −12.0 %, batch delete −7.8 %, batch
  update −4.9 % - each of the three `main` runs below both runs before; the interleaved session above (A, B, A, B)
  showed −0.2 %, −3.5 % and +1.8 %. Measured to the bottom in "The new index and writes" below: the index costs a
  batch create or delete 1-4.5 %, and this session's −12 % was contention. The other scenarios: −3.9 % to +1.6 %.

### The new index and writes

Does `{type, createdAt, _id}` in place of `{type}` cost a write, and how much? Measured 2026-10-09 00:16-06:05,
three ways, each with the other variable held still.

**Conditions, all of them.** Builds: A = `65be8c03` (the commit before #292, index `{type}`), B = `main` `6493d6c2`,
both release (every library release, no `COR_T_ON` marker), built from clean checkouts of every library's
`main`; **B-old** = the B binary with the old indexes put back by hand after every broker start (`type_1_createdAt_1__id_1`
dropped, `{type}` created) - the index without the rest of the binary. MongoDB 8.2.12 in its container (`mongo:8.2`,
port mapping), its cpuset set per view with `docker update --cpuset-cpus`; wrk on CPUs 2-7 in every view; a fresh
database and broker before every scenario (perfRun's resets: before every repeat for the creates and the batch
delete, whose pool is 100 000 entities). perfRun's entity, fixture and Lua scripts, 10 s x 5 repeats per scenario;
a run's figure is the median of its repeats. CPU: governor `powersave` (amd-pstate-epp, `balance_performance`), idle
states deep - not changed (it needs root). Interleaved A, B, B-old, A, ... in every view.

The three views:

- **broker-isolated** - broker on CPUs 0-1, MongoDB on 8 physical cores of its own (8-15 and their siblings).
- **mongo-bound** - broker on CPUs 0-1, MongoDB on 2 physical cores (8-9 and siblings): the database limits.
- **shared-8** - broker AND MongoDB on the same 8 physical cores (8-15 and siblings), not pinned against each other:
  what one machine of 8 cores does with both. With it, for reference, `corDB` on the same 8 cores (B, `--dbDir`
  on the local NVMe disk).

**MongoDB alone** - `mongosh`, the stored form of perfRun's entity, 100 000 documents preloaded, 50 000 inserted,
updated (`$set` of a value and `modifiedAt`) and deleted in batches, old and new index sets alternating, 7 repeats
each, one client. Documents/s:

| | old (`{type}`) | new (`{type,createdAt,_id}`) | new vs old, 95 % |
|---|---:|---:|---:|
| insert, batches of 20 | 37 207 | 37 286 | +0.2 % ± 1.7 |
| update, batches of 20 | 31 398 | 31 570 | +0.5 % ± 1.2 |
| delete, batches of 20 | 43 758 | 43 060 | -1.6 % ± 1.0 |
| insert, batches of 100 | 53 952 | 52 845 | -2.1 % ± 1.8 |
| update, batches of 100 | 41 570 | 41 448 | -0.3 % ± 0.9 |
| delete, batches of 100 | 75 011 | 73 177 | -2.4 % ± 1.6 |

The `{type}` index of 100 000 entities of one type is 40-48 KiB (one key, prefix-compressed); the new one 256 KiB.

**Throughput, requests/s** (mean of the runs ± their sd; the interval is 95 %, Welch with a t for the small n):

*shared-8* (three runs each; `corDB` two):

| requests/s | A | B | Bold | BcorDB | B vs A | B-old vs A |
|---|---:|---:|---:|---:|---:|---:|
| batch create (20) | 12 066 ± 0.1 % | 11 519 ± 0.5 % | 11 967 ± 0.3 % | 19 884 ± 0.1 % | -4.5 % ± 0.8 | -0.8 % ± 0.5 |
| batch update (20) | 2 898 ± 0.1 % | 2 891 ± 0.2 % | 2 901 ± 0.6 % | 38 828 ± 0.1 % | -0.2 % ± 0.4 | +0.1 % ± 1.0 |
| batch delete (20) | 7 161 ± 0.5 % | 6 881 ± 0.4 % | 7 153 ± 0.4 % | 67 829 ± 7.8 % | -3.9 % ± 1.0 | -0.1 % ± 1.0 |
| create, c50 | 57 173 ± 0.4 % | 56 687 ± 0.4 % | 56 998 ± 0.5 % | 93 304 ± 0.2 % | -0.8 % ± 0.9 | -0.3 % ± 1.0 |
| create, c1 | 6 468 ± 0.3 % | 6 520 ± 1.2 % | 6 504 ± 0.4 % | 26 618 ± 0.6 % | +0.8 % ± 2.0 | +0.6 % ± 0.8 |
| `PATCH`, c50 (control) | 25 464 ± 0.3 % | 25 407 ± 0.4 % | 25 498 ± 0.4 % | 137 386 ± 1.6 % | -0.2 % ± 0.9 | +0.1 % ± 0.8 |
| query, 1 % type of 100 000, `limit=20` | 1 110 ± 0.0 % | 26 228 ± 0.4 % | 1 110 ± 0.1 % | 57 850 ± 0.0 % | ×23.6 | ×1.0 |
| query `limit=20` | 25 755 ± 0.1 % | 26 512 ± 0.1 % | 25 836 ± 0.2 % | 85 261 ± 0.3 % | +2.9 % ± 0.3 | +0.3 % ± 0.4 |
| `GET /entities/{id}` | 72 390 ± 0.4 % | 72 328 ± 0.2 % | 71 970 ± 0.4 % | 497 354 ± 0.2 % | -0.1 % ± 0.7 | -0.6 % ± 0.9 |

*mongo-bound* (three runs each):

| requests/s | A | B | Bold | B vs A | B-old vs A |
|---|---:|---:|---:|---:|---:|
| batch create (20) | 9 142 ± 0.3 % | 8 851 ± 0.2 % | 9 098 ± 0.3 % | -3.2 % ± 0.5 | -0.5 % ± 0.6 |
| batch update (20) | 1 867 ± 0.0 % | 1 868 ± 0.1 % | 1 867 ± 0.2 % | +0.1 % ± 0.2 | +0.0 % ± 0.3 |
| batch delete (20) | 4 299 ± 0.2 % | 4 249 ± 0.2 % | 4 287 ± 0.7 % | -1.2 % ± 0.4 | -0.3 % ± 1.2 |
| create, c50 | 38 246 ± 0.5 % | 37 982 ± 0.3 % | 37 873 ± 0.2 % | -0.7 % ± 1.0 | -1.0 % ± 0.9 |
| create, c1 | 8 473 ± 0.5 % | 8 404 ± 0.6 % | 8 469 ± 0.4 % | -0.8 % ± 1.3 | -0.0 % ± 1.0 |
| `PATCH`, c50 (control) | 17 243 ± 0.4 % | 17 276 ± 0.2 % | 17 284 ± 0.4 % | +0.2 % ± 0.8 | +0.2 % ± 1.0 |
| query, 1 % type of 100 000, `limit=20` | 1 675 ± 0.1 % | 12 173 ± 0.6 % | 1 678 ± 0.1 % | ×7.3 | ×1.0 |
| query `limit=20` | 12 097 ± 0.7 % | 12 144 ± 1.1 % | 12 194 ± 0.8 % | +0.4 % ± 2.1 | +0.8 % ± 1.7 |
| `GET /entities/{id}` | 41 699 ± 1.4 % | 42 013 ± 0.3 % | 42 067 ± 0.2 % | +0.8 % ± 2.3 | +0.9 % ± 2.2 |

*broker-isolated* (two runs each in this series - and three each, write scenarios only, in a first series the same
night, 00:23-01:29):

| requests/s | A | B | Bold | B vs A | B-old vs A |
|---|---:|---:|---:|---:|---:|
| batch create (20) | 10 854 ± 2.6 % | 10 728 ± 3.4 % | 10 975 ± 0.4 % | -1.2 % ± 12.8 | +1.1 % ± 8.1 |
| batch update (20) | 3 232 ± 2.1 % | 3 191 ± 5.7 % | 3 296 ± 0.4 % | -1.3 % ± 18.3 | +2.0 % ± 6.5 |
| batch delete (20) | 6 932 ± 2.4 % | 6 879 ± 0.4 % | 6 979 ± 1.2 % | -0.8 % ± 7.3 | +0.7 % ± 8.1 |
| create, c50 | 36 472 ± 3.3 % | 36 472 ± 3.4 % | 37 234 ± 1.1 % | +0.0 % ± 14.4 | +2.1 % ± 10.7 |
| create, c1 | 8 328 ± 1.1 % | 8 292 ± 1.8 % | 8 422 ± 0.8 % | -0.4 % ± 6.3 | +1.1 % ± 4.2 |
| `PATCH`, c50 (control) | 21 485 ± 1.8 % | 21 445 ± 2.5 % | 21 640 ± 0.3 % | -0.2 % ± 9.4 | +0.7 % ± 5.6 |
| query, 1 % type of 100 000, `limit=20` | 1 698 ± 1.3 % | 11 798 ± 0.1 % | 1 710 ± 0.1 % | ×6.9 | ×1.0 |
| query `limit=20` | 11 756 ± 2.4 % | 11 912 ± 0.1 % | 11 870 ± 0.8 % | +1.3 % ± 7.3 | +1.0 % ± 7.8 |
| `GET /entities/{id}` | 39 086 ± 3.6 % | 39 982 ± 1.2 % | 39 848 ± 0.4 % | +2.3 % ± 11.7 | +1.9 % ± 11.1 |

| scenario | A (before #292) | B (`main`) | B + old indexes | B vs A | B+old vs A | B vs B+old |
|---|---:|---:|---:|---:|---:|---:|
| batch create (20), c50 | 10 935 ± 0.5 % (10 894, 10 990, 10 920) | 10 735 ± 0.4 % (10 680, 10 764, 10 761) | 10 702 ± 4.1 % (11 077, 10 810, 10 220) | -1.8 % ± 1.0 | -2.1 % ± 6.5 | +0.3 % ± 6.6 |
| batch update (20), c50 | 3 222 ± 2.8 % (3 128, 3 311, 3 227) | 3 227 ± 1.4 % (3 278, 3 213, 3 189) | 3 265 ± 3.4 % (3 347, 3 308, 3 141) | +0.1 % ± 5.1 | +1.3 % ± 7.1 | -1.2 % ± 5.8 |
| batch delete (20), c50 | 6 938 ± 0.9 % (6 991, 6 870, 6 952) | 6 810 ± 0.2 % (6 801, 6 827, 6 803) | 6 951 ± 2.1 % (7 104, 6 931, 6 818) | -1.8 % ± 1.5 | +0.2 % ± 3.6 | -2.0 % ± 3.3 |
| create, c50 | 36 891 ± 1.3 % (36 358, 37 250, 37 066) | 36 817 ± 1.0 % (37 165, 36 435, 36 850) | 36 170 ± 4.1 % (37 547, 36 345, 34 619) | -0.2 % ± 2.6 | -2.0 % ± 6.7 | +1.8 % ± 6.7 |
| create, c1 | 8 415 ± 0.5 % (8 455, 8 421, 8 369) | 8 359 ± 0.5 % (8 403, 8 329, 8 346) | 8 332 ± 2.2 % (8 438, 8 433, 8 124) | -0.7 % ± 1.1 | -1.0 % ± 3.5 | +0.3 % ± 3.5 |
| `PATCH .../attrs`, c50 (control) | 21 395 ± 2.4 % (20 883, 21 905, 21 397) | 21 603 ± 0.9 % (21 660, 21 378, 21 770) | 21 499 ± 2.0 % (21 721, 21 781, 20 994) | +1.0 % ± 4.1 | +0.5 % ± 5.0 | +0.5 % ± 3.6 |

**MongoDB CPU per written entity** (reads: per request) - the container's cgroup `cpu.stat` `usage_usec` before and
after every measured repeat, divided by the entities the repeat wrote (requests x batch size; the batch delete: its
pool). Mean ± sd over all repeats:

*shared-8*:

| MongoDB CPU, µs per entity (reads: per request) | A | B | Bold | B vs A | B vs B-old |
|---|---:|---:|---:|---:|---:|
| batch create (20) | 39.0 ± 0.4 | 41.9 ± 0.4 | 39.3 ± 0.6 | +7.4 % | +6.6 % |
| batch update (20) | 175.6 ± 0.6 | 176.2 ± 1.2 | 176.0 ± 1.7 | +0.4 % | +0.1 % |
| batch delete (20) | 89.2 ± 4.2 | 93.4 ± 3.8 | 89.0 ± 3.8 | +4.7 % | +4.9 % |
| create, c50 | 140.3 ± 1.7 | 142.0 ± 2.0 | 140.6 ± 1.6 | +1.2 % | +1.0 % |
| create, c1 | 71.1 ± 7.3 | 71.5 ± 7.0 | 71.6 ± 9.3 | +0.6 % | -0.2 % |
| `PATCH`, c50 (control) | 351.5 ± 1.7 | 352.4 ± 2.0 | 351.3 ± 2.7 | +0.3 % | +0.3 % |
| query, 1 % type of 100 000, `limit=20` | 13766.9 ± 90.2 | 234.3 ± 2.5 | 13788.0 ± 70.6 | -98.3 % | -98.3 % |
| query `limit=20` | 245.4 ± 3.8 | 230.5 ± 4.0 | 245.4 ± 4.3 | -6.1 % | -6.0 % |
| `GET /entities/{id}` | 97.2 ± 0.8 | 97.2 ± 0.5 | 97.5 ± 0.6 | +0.1 % | -0.3 % |

*mongo-bound*:

| MongoDB CPU, µs per entity (reads: per request) | A | B | Bold | B vs A | B vs B-old |
|---|---:|---:|---:|---:|---:|
| batch create (20) | 21.6 ± 0.4 | 22.2 ± 0.2 | 21.7 ± 0.4 | +2.8 % | +2.3 % |
| batch update (20) | 107.2 ± 0.3 | 107.1 ± 0.3 | 107.2 ± 0.3 | -0.1 % | -0.0 % |
| batch delete (20) | 46.1 ± 0.3 | 47.0 ± 0.4 | 47.5 ± 2.8 | +2.0 % | -1.1 % |
| create, c50 | 92.8 ± 1.9 | 93.8 ± 1.6 | 93.5 ± 2.0 | +1.1 % | +0.3 % |
| create, c1 | 59.3 ± 4.8 | 59.9 ± 6.0 | 58.6 ± 4.7 | +0.9 % | +2.3 % |
| `PATCH`, c50 (control) | 231.5 ± 1.1 | 231.3 ± 1.1 | 231.1 ± 1.4 | -0.1 % | +0.1 % |
| query, 1 % type of 100 000, `limit=20` | 2388.8 ± 5.9 | 145.6 ± 1.3 | 2385.0 ± 7.2 | -93.9 % | -93.9 % |
| query `limit=20` | 145.4 ± 3.0 | 140.5 ± 1.0 | 143.9 ± 1.5 | -3.4 % | -2.4 % |
| `GET /entities/{id}` | 61.7 ± 0.4 | 61.2 ± 0.3 | 61.4 ± 0.3 | -0.8 % | -0.4 % |

*broker-isolated*:

| MongoDB CPU, µs per entity (reads: per request) | A | B | Bold | B vs A | B vs B-old |
|---|---:|---:|---:|---:|---:|
| batch create (20) | 21.7 ± 0.7 | 24.0 ± 1.0 | 21.4 ± 0.5 | +10.6 % | +12.3 % |
| batch update (20) | 137.2 ± 1.9 | 137.1 ± 1.6 | 133.6 ± 1.2 | -0.1 % | +2.7 % |
| batch delete (20) | 88.9 ± 2.8 | 95.8 ± 3.4 | 85.3 ± 5.0 | +7.8 % | +12.3 % |
| create, c50 | 91.5 ± 2.9 | 93.2 ± 1.8 | 90.6 ± 2.3 | +2.0 % | +2.9 % |
| create, c1 | 60.6 ± 6.5 | 60.0 ± 5.6 | 59.5 ± 3.1 | -1.0 % | +0.7 % |
| `PATCH`, c50 (control) | 256.1 ± 3.1 | 258.9 ± 5.5 | 254.0 ± 0.6 | +1.1 % | +1.9 % |
| query, 1 % type of 100 000, `limit=20` | 3913.0 ± 49.9 | 145.2 ± 1.2 | 3886.3 ± 18.8 | -96.3 % | -96.3 % |
| query `limit=20` | 144.7 ± 1.2 | 141.2 ± 1.3 | 144.2 ± 2.2 | -2.4 % | -2.1 % |
| `GET /entities/{id}` | 62.4 ± 1.1 | 61.1 ± 0.3 | 60.9 ± 0.2 | -2.1 % | +0.3 % |

The broker's own CPU per entity does not move with the index in any view (tables in the run logs; batch create
9.0 / 9.1 / 8.9 µs per entity for A / B / B-old, broker-isolated).

**What it shows:**

- **The index costs batch create and batch delete, nothing else.** Throughput, B against A: batch create −4.5 % ±
  0.8 (shared-8), −3.2 % ± 0.5 (mongo-bound), −1.2 % / −1.8 % ± 1.0 (broker-isolated, the two series); batch delete
  −3.9 % ± 1.0, −1.2 % ± 0.4, −0.8 %. B-old against A in the same views: −0.8 %, −0.5 %, +1.1 % (batch create) and
  −0.1 %, −0.3 %, +0.7 % (batch delete) - the old indexes on the new binary give the old numbers back, so the cost
  is the index. Batch update, create (c50 and c1) and the `PATCH` control: within ±1.5 % in every view.
- **In MongoDB's CPU**: +2.9 µs per batch-created entity (39.0 -> 41.9, +7.4 %, shared-8; +2.8 % mongo-bound,
  +10.6 % broker-isolated) and +4 µs per batch-deleted one (+4.7 %, +2.0 %, +7.8 %); a single create +1-2 %, an update
  nothing - an update does not change an indexed field.
- **The reads, the same runs**: a page of a 1 % type ×6.9 (broker-isolated), ×7.3 (mongo-bound), **×23.6
  (shared-8: 1 110 -> 26 228 requests/s)**; MongoDB CPU for that page 13 767 -> 234 µs (shared-8). A query of the
  fixture's one type (`limit=20`): +2.9 % ± 0.3 (shared-8), MongoDB CPU −6 % per request. Retrieve: no change.
- **One rare-type page saves the MongoDB CPU of ~4 700 batch-created entities** (13 533 µs against 2.9 µs, shared-8).
- **The session that showed batch create −12 %** (`perfRun.sh` unchanged, MongoDB unpinned, wrk on 14 cores beside
  it) measured contention, not the index: its three `main` runs alone spread 10.7 %, and no view here - each with
  MongoDB, the broker and wrk on cores of their own - shows more than −4.5 % ± 0.8.

**Conclusion:** the index costs a batch write 1-4.5 % of throughput (the most where broker and database share the
cores) and 3-4 µs of MongoDB CPU per entity, and nothing on single-entity writes, updates and reads; it makes a page
of a rare type 7-24x faster and an ordinary query by type up to 3 % faster. It stays.

`corDB` on the same 8 cores, for reference: batch create 19 884 requests/s against 11 519 (mongoc, B), batch update
38 828 against 2 891, `PATCH` 137 386 against 25 407, retrieve 497 354 against 72 328 (wrk on six cores - for the
largest `corDB` rates the load generator may be the limit).

### mongoc: every scenario, 2026-10-08

`test/perf/perfRun.sh mongoc` on `main` `6493d6c2` (with the `{type, createdAt, _id}` index), three runs
(2026-10-08 23:21 - 2026-10-09 00:08, between them the runs of the commit before - above):

| | |
|---|---|
| Build | `make release`, every library release (no `COR_T_ON` marker in the broker or `mongoc.so`), no PGO, libmicrohttpd |
| Machine | AMD Ryzen 9 8940HX, governor `powersave`, idle states deep |
| MongoDB | 8.2.12 in a container (`mongo:8.2`) through a port mapping (`-p 27017:27017`), not pinned |
| Load | `PERF_BROKER_CORES=2` (broker on CPUs 0-1, wrk on 2-15), `PERF_DURATION=4s`, `PERF_REPEATS=3`; the delete scenarios' wrk ceiling cut from 60 s to 3 s as in "The request arena's sizes" |

Requests/s; the spread is (largest - smallest) / mean of the three:

| scenario | run 1 | run 2 | run 3 | spread | p99 ms, runs 1 / 2 / 3 |
|---|---:|---:|---:|---:|---:|
| query `limit=1`, c50 | 27 106 | 28 446 | 29 033 | 6.8 % | 3.75 / 3.62 / 3.18 |
| query `limit=20`, c50 | 11 264 | 11 416 | 11 438 | 1.5 % | 7.33 / 7.17 / 6.86 |
| query `limit=20`, c200 | 10 822 | 11 353 | 11 405 | 5.2 % | 21.96 / 20.81 / 21.25 |
| query `limit=100`, c50 | 3 088 | 3 193 | 3 230 | 4.5 % | 35.28 / 27.17 / 36.34 |
| `GET /entities/{id}`, c50 | 33 741 | 35 531 | 35 721 | 5.7 % | 3.62 / 3.25 / 3.21 |
| `PATCH .../attrs`, c50 | 17 266 | 18 335 | 18 199 | 6.0 % | 4.14 / 3.83 / 3.88 |
| `PATCH .../attrs`, c1 | 3 178 | 3 182 | 3 468 | 8.9 % | 2.03 / 0.99 / 0.67 |
| merge, c50 | 21 426 | 22 665 | 22 987 | 7.0 % | 3.87 / 3.50 / 3.99 |
| batch update (20), c50 | 1 551 | 1 649 | 1 707 | 9.5 % | 44.49 / 33.98 / 32.89 |
| create, c50 | 30 825 | 33 287 | 33 651 | 8.7 % | 4.14 / 4.13 / 3.68 |
| create, c1 | 6 808 | 7 410 | 7 410 | 8.4 % | 1.75 / 1.10 / 0.26 |
| batch create (20), c50 | 6 005 | 6 222 | 6 682 | 10.7 % | 26.91 / 53.19 / 40.31 |
| `DELETE`, c50 | 23 421 | 25 563 | 26 005 | 10.3 % | 6.50 / 4.64 / 4.52 |
| batch delete (20), c50 | 3 091 | 3 165 | 3 251 | 5.0 % | 24.55 / 26.45 / 25.32 |
| `PATCH`, 1 subscriber, c50 | 11 051 | 11 721 | 11 873 | 7.1 % | 9.28 / 9.07 / 7.10 |
| `PATCH`, ~210 subscriptions, c50 | 10 871 | 11 277 | 11 357 | 4.4 % | 8.70 / 8.26 / 7.95 |
| 100 000 entities, 1 % type: `limit=20` | 11 377 | 11 369 | 11 499 | 1.1 % | 7.06 / 6.95 / 6.92 |
| 100 000, 1 %: `limit=100` | 3 157 | 3 177 | 3 200 | 1.4 % | 28.00 / 29.10 / 34.09 |
| 100 000, 1 %: `limit=20&offset=500` | 3 653 | 3 928 | 3 910 | 7.2 % | 20.20 / 20.09 / 20.07 |
| 300 000 entities, 1 % type: `limit=20` | 11 383 | 11 495 | 11 426 | 1.0 % | 6.93 / 7.07 / 7.76 |
| 300 000, 1 %: `limit=20&offset=1000` | 1 953 | 1 960 | 1 750 | 11.1 % | 34.22 / 32.98 / 39.03 |
| 300 000, 0.1 % type: `limit=20` | 11 219 | 11 491 | 11 265 | 2.4 % | 7.00 / 7.07 / 6.95 |
| 300 000, 0.1 %: `limit=20&offset=200` | 6 649 | 7 022 | 6 801 | 5.5 % | 10.92 / 10.87 / 11.12 |

Two broker cores and an unpinned MongoDB behind a port mapping: not the conditions of the per-core
tables above (one core, PGO, MongoDB on the host network and pinned), and not to be compared with them.
One run of the commit before, in this session, ended at the delete pool's fill (`curl` exit 52, an empty
reply) and was run again; the runs of `main` did not.

### What automatic EntityMaps cost

Since #298 (roadmap § 13) a local `GET /ngsi-ld/v1/entities` whose first page (`offset` 0, no `orderBy`) has more
matches than `limit` also scans the ids of every match and stores them as an EntityMap (`--entityMapMemory`,
default 64 MiB, least recently used first out); the pages after it are served from the map
([EntityMaps](installation.md#entitymaps)). Measured 2026-10-09 12:09-13:21.

| | |
|---|---|
| Builds | A = `2fcadbb3` (`main` before #298) with corNgsild `078215d`, corDB `41e9232`; B = `f73ee184` (#298) with corNgsild `e86fb5b`, corDB `51d0cd3`; every other library at its `main`. Release, every library release, no `COR_T_ON` marker; the release `admin.so` loaded in both (for `/metrics`) |
| Machine | AMD Ryzen 9 8940HX, governor `powersave`, idle states deep |
| Placement | broker on CPUs 0-1, wrk on 2-7, MongoDB 8.2.12 (container, port mapping) pinned to 8-15 and their siblings - the broker-isolated view of "The new index and writes" |
| Stores | perfRun's: the 100-entity fixture; 100 000 entities, 1 % type `Rare`; 300 000 entities, 1 % type R1 and 0.1 % type R01. A fresh database per store, a fresh broker before every scenario (no map carried over). corDB with `--dbDir` on the local NVMe disk |
| Load | perfRun's request shapes, 50 connections unless noted, 10 s x 5 per scenario, a run's figure the median of its repeats |
| Runs | mongoc A, mongoc B, corDB A, corDB B, mongoc A (stopped there: the differences are many times the spread) |

wrk asks every request at `offset` 0 again - the first page, many times, which is what an application polling
a query does. Nothing asks for page two, so on B every one of those requests made a map that nobody used.

**mongoc**, requests/s (A: mean of two runs ± their sd; B: one run), p99, CPU per request:

| requests/s | A (before #298) | B (`main`, automatic EntityMaps) | B vs A | p99 ms, A / B | broker CPU µs/request, A / B | MongoDB CPU µs/request, A / B |
|---|---:|---:|---:|---:|---:|---:|
| query `limit=1` | 33 882 ± 3.2 % | 3 152 | -90.7 % | 2.98 / 42.03 | 59 / 413 | 148 / 287 |
| query `limit=20` | 11 652 ± 0.3 % | 2 954 | -74.6 % | 6.85 / 46.58 | 171 / 519 | 142 / 303 |
| query `limit=20`, c200 | 11 754 ± 2.5 % | 2 921 | -75.1 % | 19.98 / 98.56 | 170 / 529 | 143 / 304 |
| query `limit=100` (one page: no map) | 3 194 ± 3.9 % | 3 239 | +1.4 % | 32.47 / 29.59 | 626 / 617 | 256 / 255 |
| `GET /entities/{id}` | 39 610 ± 2.8 % | 40 247 | +1.6 % | 2.95 / 2.86 | 51 / 49 | 61 / 61 |
| `PATCH`, c50 (control) | 21 400 ± 1.9 % | 21 536 | +0.6 % | 3.24 / 3.19 | 94 / 93 | 258 / 252 |
| 100 000, 1 % type: `limit=20` | 11 695 ± 1.2 % | 2 225 | -81.0 % | 6.75 / 28.01 | 171 / 378 | 148 / 3531 |
| 100 000, 1 %: `limit=100` | 3 292 ± 1.8 % | 2 099 | -36.2 % | 30.08 / 29.22 | 608 / 889 | 276 / 3726 |
| 100 000, 1 %: `offset=500` (no map) | 7 328 ± 5.5 % | 7 601 | +3.7 % | 9.88 / 9.55 | 197 / 190 | 1157 / 1053 |
| 100 000, 1 %: `offset=500` - B through the first page's map | 7 326 ± 5.6 % | 3 843 | -47.5 % | 9.89 / 16.65 | 192 / 521 | 1160 / 1350 |
| 300 000, 1 % type: `limit=20` | 11 894 ± 2.0 % | 574 | -95.2 % | 6.68 / 106.07 | 168 / 580 | 148 / 22590 |
| 300 000, 1 %: `offset=1000` (no map) | 3 964 ± 0.4 % | 3 123 | -21.2 % | 17.73 / 20.09 | 186 / 181 | 2135 / 5097 |
| 300 000, 0.1 % type: `limit=20` | 11 908 ± 2.2 % | 5 113 | -57.1 % | 6.70 / 14.88 | 168 / 376 | 151 / 1892 |
| 300 000, 0.1 %: `offset=200` (no map) | 11 396 ± 0.9 % | 11 460 | +0.6 % | 6.84 / 6.75 | 175 / 174 | 346 / 343 |

**corDB** (`--dbDir`), one run each:

| requests/s | A (before #298) | B (`main`, automatic EntityMaps) | B vs A | p99 ms, A / B | broker CPU µs/request, A / B |
|---|---:|---:|---:|---:|---:|
| query `limit=1` | 153 855 | 9 070 | -94.1 % | 0.38 / 6.51 | 13 / 138 |
| query `limit=20` | 22 656 | 4 001 | -82.3 % | 2.53 / 14.48 | 88 / 348 |
| query `limit=20`, c200 | 22 498 | 4 067 | -81.9 % | 9.42 / 55.74 | 89 / 345 |
| query `limit=100` (one page: no map) | 5 075 | 5 102 | +0.5 % | 10.36 / 11.06 | 394 / 392 |
| `GET /entities/{id}` | 169 745 | 173 549 | +2.2 % | 0.32 / 0.31 | 12 / 12 |
| `PATCH`, c50 (control) | 182 338 | 180 195 | -1.2 % | 0.47 / 0.44 | 11 / 11 |
| 100 000, 1 % type: `limit=20` | 16 181 | 232 | -98.6 % | 3.31 / 284.44 | 123 / 8541 |
| 100 000, 1 %: `limit=100` | 3 617 | 221 | -93.9 % | 14.49 / 299.00 | 553 / 8977 |
| 100 000, 1 %: `offset=500` (no map) | 932 | 920 | -1.3 % | 62.48 / 67.42 | 2139 / 2253 |
| 100 000, 1 %: `offset=500` - B through the first page's map | 869 | 21 832 | +2412.3 % | 65.70 / 2.59 | 2308 / 91 |
| 300 000, 1 % type: `limit=20` | 16 372 | 66 | -99.6 % | 3.25 / 1298.46 | 122 / 30082 |
| 300 000, 1 %: `offset=1000` (no map) | 236 | 241 | +2.1 % | 287.12 / 268.39 | 8530 / 8306 |
| 300 000, 0.1 % type: `limit=20` | 4 975 | 66 | -98.7 % | 10.64 / 1299.22 | 402 / 30360 |
| 300 000, 0.1 %: `offset=200` (no map) | 107 | 108 | +0.9 % | 739.82 / 735.77 | 18585 / 18535 |

The deep page "through the first page's map": on B, the first page's `Link: rel="next"` (`?entityMap=<id>&...`) with
its `offset` set to 500, asked repeatedly - a page served from the map; on A (no map) the plain `offset=500` query.

**The EntityMap store after each scenario** (B; `ngsild_entity_map_store_size` and `ngsild_entity_map_bytes` from
`/metrics`) and the broker's RSS, mongoc:

| after | ngsild_entity_map_store_size | ngsild_entity_map_bytes | RSS A, MiB | RSS B, MiB |
|---|---:|---:|---:|---:|
| query `limit=1` | 8 995 | 67 102 700 | 49 | 114 |
| query `limit=20` | 8 993 | 67 087 780 | 54 | 128 |
| query `limit=20`, c200 | 8 994 | 67 100 511 | 78 | 134 |
| query `limit=100` (one page: no map) | 0 | 0 | 77 | 71 |
| `GET /entities/{id}` | 0 | 0 | 49 | 48 |
| 100 000, 1 % type: `limit=20` | 903 | 67 052 265 | 56 | 142 |
| 100 000, 1 %: `limit=100` | 903 | 67 052 516 | 89 | 175 |
| 100 000, 1 %: `offset=500` (no map) | 0 | 0 | 55 | 55 |
| 100 000, 1 %: `offset=500` - B through the first page's map | 1 | 74 255 | 55 | 52 |
| 300 000, 1 % type: `limit=20` | 299 | 67 051 049 | 60 | 162 |
| 300 000, 1 %: `offset=1000` (no map) | 0 | 0 | 53 | 52 |
| 300 000, 0.1 % type: `limit=20` | 2 950 | 67 106 600 | 57 | 147 |
| 300 000, 0.1 %: `offset=200` (no map) | 0 | 0 | 57 | 57 |

corDB:

| after | ngsild_entity_map_store_size | ngsild_entity_map_bytes | RSS A, MiB | RSS B, MiB |
|---|---:|---:|---:|---:|
| query `limit=1` | 8 995 | 67 102 700 | 17 | 77 |
| query `limit=20` | 8 995 | 67 102 700 | 18 | 76 |
| query `limit=20`, c200 | 8 995 | 67 102 700 | 22 | 81 |
| query `limit=100` (one page: no map) | 0 | 0 | 18 | 18 |
| `GET /entities/{id}` | 0 | 0 | 17 | 17 |
| 100 000, 1 % type: `limit=20` | 902 | 66 978 010 | 208 | 284 |
| 100 000, 1 %: `limit=100` | 903 | 67 052 265 | 209 | 284 |
| 100 000, 1 %: `offset=500` (no map) | 0 | 0 | 209 | 209 |
| 100 000, 1 %: `offset=500` - B through the first page's map | 1 | 74 255 | 209 | 208 |
| 300 000, 1 % type: `limit=20` | 299 | 67 051 049 | 590 | 670 |
| 300 000, 1 %: `offset=1000` (no map) | 0 | 0 | 591 | 591 |
| 300 000, 0.1 % type: `limit=20` | 2 949 | 67 083 852 | 590 | 677 |
| 300 000, 0.1 %: `offset=200` (no map) | 0 | 0 | 591 | 591 |

- **Every first page with more matches than `limit` costs 36-99.6 %** of its throughput: mongoc `limit=1` 33 882 ->
  3 152, `limit=20` 11 652 -> 2 954; the 1 % type of the 300 000-entity store 11 894 -> 574; corDB `limit=1` 153 855
  -> 9 070, `limit=20` 22 656 -> 4 001, the 1 % type of 300 000 16 372 -> 66 (p99 3.3 ms -> 1.3 s, 30 ms of broker
  CPU per request). The scan of every match is paid on every first page; the map store stays at its 64 MiB cap
  (~9 000 maps of ~7.5 KB for 100 matches, ~900 of ~74 KB for 1 000, ~300 of ~224 KB for 3 000), and the broker's RSS
  grows by 60-90 MiB.
- **Unchanged** where no map is made: a query of one page (`limit=100` over 100 matches), retrieve, `PATCH` (the
  control), and a query starting at another `offset` - within ±4 % on both stores, but one: mongoc, the 1 % type of
  300 000 at `offset=1000`, −21.2 % (3 964 -> 3 123), MongoDB CPU per request 2 135 -> 5 097 µs. Not B's: A and B send
  MongoDB the same command for it, and on one store they answer it at the same rate - each run here built its own
  store, and the same query's cost differs from one such store to the next by that much (see the next section).
- **A page through the map**: corDB ×25 (869 -> 21 832 requests/s for `offset=500` of the 1 % type) - corDB's own
  deep `offset` is a scan, the map is a slice; mongoc −47.5 % (7 326 -> 3 843) - the page fetched its 20 entities
  with 20 queries, one after the other (broker CPU 192 -> 521 µs, MongoDB CPU 1 160 -> 1 350 µs per request).

**Conclusion:** as merged, automatic EntityMaps cost every first page of a query with more matches than `limit`
36-99.6 % of its throughput, on both stores, and pay off only for a client that pages deep on corDB.

### What automatic EntityMaps cost - and the fix

The fix (branch `fix/entitymap-cost`):

- **No automatic map for a local query** by default: `--autoEntityMaps none|distributed|all` (default
  `distributed` - a distributed query has no correct second page without one); `entityMap=true` and
  `/entityMaps` as before ([EntityMaps](installation.md#entitymaps)).
- **A page of a map fetches its entities one call per source**: the local ones in one call to the store
  (`db.entityBulkRetrieve` - MongoDB one `{_id: {$in: [...]}}` query, corDB the id index under one read lock), each
  Context Source's in one `GET /entities?type=..&id=a,b,c` (all sources in parallel) - not one fetch per entity.
- **A local query pages by position** ([Pagination](installation.md#pagination)): its links name the page's last /
  first entity (`pageAfter` / `pageBefore` = `<createdAt>,<id>`), and the store reads from there - MongoDB an index
  range on `{type, createdAt, _id}` (`explain()`: no `SKIP`, no `SORT`, 22 keys and 22 documents for a page of 20 at
  depth 1 000 of the 1 % type of 300 000), corDB the walk from that entity (the id index).

Measured 2026-10-09 15:26-16:40.

| | |
|---|---|
| Builds | A = `2fcadbb3` and B = `f73ee184` as above; C = `fix/entitymap-cost` `260e3e27` with corNgsild `72b8894`, corDB `cb3a3e1`, every other library as B. Release, every library release; the release `admin.so` loaded (for `/metrics`) |
| Machine, placement | as above: broker on CPUs 0-1, wrk on 2-7, MongoDB 8.2.12 pinned to 8-15 and their siblings |
| Stores | the same three, corDB with `--dbDir` on the local NVMe disk - but **each store created once** (by A) and every variant measured on it, scenario by scenario, the order of A / B / C rotating from one scenario to the next. The runs above built a store per variant, and the cost of the same MongoDB query differs from one freshly built store to the next by more than the differences looked for here (below) |
| Load | 10 s x 5 per scenario and variant, a fresh broker before each, a run's figure the median of its repeats; 50 connections |
| Runs | mongoc twice (the mean, ± half the spread of the two), corDB once |
| Deep pages | by `offset` (A, B, C - C pages a client's own `offset` as A does); through a map the client asked for (`entityMap=true`, then the map's page at `offset=500` - not A: its requested map froze `limit` + 1 ids); by position (C: the first page's `next` followed 25 / 50 times, then that page asked repeatedly) |

**mongoc**

| requests/s | A (before #298) | B (#298) | C (the fix) | C vs A | p99 ms A / B / C | broker CPU µs/request A / B / C | MongoDB CPU µs/request A / B / C |
|---|---:|---:|---:|---:|---:|---:|---:|
| query `limit=1` (100 entities) | 30 920 ± 2.2 % | 3 042 ± 2.3 % | 30 164 ± 3.1 % | -2.4 % | 3.51 / 42.51 / 3.31 | 65 / 430 / 66 | 216 / 353 / 211 |
| query `limit=20` (100 entities) | 11 354 ± 2.0 % | 2 802 ± 1.0 % | 11 192 ± 3.1 % | -1.4 % | 7.06 / 49.23 / 7.23 | 176 / 537 / 179 | 185 / 372 / 184 |
| 100 000, 1 % type: first page `limit=20` | 11 440 ± 1.7 % | 2 032 ± 4.8 % | 11 381 ± 1.6 % | -0.5 % | 7.09 / 30.55 / 6.92 | 175 / 395 / 176 | 192 / 4269 / 191 |
| 100 000, 1 %: `offset=500` | 6 914 ± 4.5 % | 6 498 ± 5.0 % | 6 143 ± 10.8 % | -11.2 % | 10.39 / 10.98 / 11.81 | 199 / 203 / 205 | 1284 / 1509 / 1852 |
| 100 000, 1 %: depth 500 through a requested map (`entityMap=true`, then its page at `offset=500`) | - | 3 098 ± 1.9 % | 11 056 ± 1.2 % | +59.9 % vs A at `offset` | - / 20.22 / 7.43 | - / 646 / 181 | - / 2630 / 213 |
| 100 000, 1 %: depth 500 by position (`next` followed 25 times: `pageAfter`) | - | - | 10 972 ± 0.4 % | ×1.6 vs A at `offset` | - / - / 7.31 | - / - / 182 | - / - / 251 |
| 300 000, 1 % type: first page `limit=20` | 11 340 ± 0.4 % | 558 ± 0.6 % | 11 393 ± 1.0 % | +0.5 % | 7.14 / 109.59 / 6.99 | 176 / 594 / 176 | 195 / 28458 / 190 |
| 300 000, 1 %: `offset=1000` | 3 396 ± 0.1 % | 3 546 ± 0.8 % | 3 485 ± 2.8 % | +2.6 % | 20.27 / 19.51 / 19.85 | 192 / 194 / 194 | 3178 / 2783 / 2965 |
| 300 000, 1 %: depth 1 000 by position (`pageAfter`) | - | - | 11 301 ± 0.2 % | ×3.3 vs A at `offset` | - / - / 7.02 | - / - / 177 | - / - / 250 |

**corDB**

| requests/s | A (before #298) | B (#298) | C (the fix) | C vs A | p99 ms A / B / C | broker CPU µs/request A / B / C |
|---|---:|---:|---:|---:|---:|---:|
| query `limit=1` (100 entities) | 149 804 | 8 854 | 142 819 | -4.7 % | 0.39 / 7.26 / 0.39 | 13 / 140 / 14 |
| query `limit=20` (100 entities) | 21 776 | 3 992 | 21 582 | -0.9 % | 2.28 / 16.54 / 2.37 | 92 / 352 / 93 |
| 100 000, 1 % type: first page `limit=20` | 15 498 | 226 | 15 562 | +0.4 % | 3.70 / 288.38 / 3.44 | 129 / 8752 / 129 |
| 100 000, 1 %: `offset=500` | 837 | 907 | 921 | +10.0 % | 73.75 / 64.53 / 65.92 | 2390 / 2207 / 2172 |
| 100 000, 1 %: depth 500 through a requested map (`entityMap=true`, then its page at `offset=500`) | - | 21 576 | 21 380 | +2454.4 % vs A at `offset` | - / 2.46 / 2.48 | - / 92 / 93 |
| 100 000, 1 %: depth 500 by position (`next` followed 25 times: `pageAfter`) | - | - | 15 549 | ×18.6 vs A at `offset` | - / - / 3.33 | - / - / 129 |
| 300 000, 1 % type: first page `limit=20` | 16 222 | 69 | 16 018 | -1.3 % | 3.52 / 1242.84 / 3.31 | 123 / 29116 / 125 |
| 300 000, 1 %: `offset=1000` | 247 | 240 | 243 | -1.6 % | 256.63 / 274.23 / 269.83 | 8099 / 8349 / 8236 |
| 300 000, 1 %: depth 1 000 by position (`pageAfter`) | - | - | 15 958 | ×64.6 vs A at `offset` | - / - / 3.45 | - / - / 125 |

The 100-entity first pages again (17:00-17:15), after the links stopped copying a value that has nothing to encode
(corNgsild `605255d`; C = coraine `ba00921a`, which also fetches a map page's remote entities per source), two runs
of each store, the store again shared by A, B and C:

| requests/s | A | B | C | C vs A | broker CPU µs/request A / B / C |
|---|---:|---:|---:|---:|---:|
| mongoc, query `limit=1` | 31 808 ± 0.8 % | 3 098 ± 0.6 % | 31 058 ± 0.6 % | -2.4 % | 63 / 423 / 64 |
| mongoc, query `limit=20` | 11 538 ± 0.0 % | 2 840 ± 0.5 % | 11 406 ± 0.6 % | -1.1 % | 173 / 531 / 175 |
| corDB, query `limit=1` | 154 156 ± 0.9 % | 8 470 ± 29.2 % | 151 214 ± 1.1 % | -1.9 % | 13 / 156 / 13 |
| corDB, query `limit=20` | 22 636 ± 0.1 % | 4 263 ± 10.8 % | 22 497 ± 0.0 % | -0.6 % | 88 / 336 / 89 |

What is left on `limit=1` (-2 %, ~0.3 µs of a 13 µs request on corDB) is the position link: the page's last
entity's `createdAt` and id, and the link built from them.

The maps: B holds 64 MiB of them after every first-page scenario (~9 000, ~900 and ~300 maps) and its RSS is
60-105 MiB above A's; C makes none (`ngsild_entity_map_store_size` 0, RSS as A's), but the one a client asks for.

- **Every first page is back at A**: mongoc within -2.4 % ... +0.5 % (the spread of the two runs is 1-3 %), corDB
  within -1.9 % ... +0.4 % (`limit=1` -4.7 % in the one full run, -1.9 % in the two runs after it).
  B: -75 % to -95 % on mongoc, -82 % to -99.6 % on corDB.
- **A deep page by position costs what the first page costs**: mongoc 10 972 (depth 500) and 11 301 requests/s
  (depth 1 000) against 6 914 and 3 396 by `offset` (×1.6, ×3.3; MongoDB CPU 250 µs per request against 1 284 and
  3 178); corDB 15 549 and 15 958 against 837 and 247 (×18.6, ×64.6) - corDB's `offset` walks every match before
  the page, the position starts at the page.
- **A page through a requested map**: mongoc 3 098 -> 11 056 requests/s (MongoDB CPU 2 630 -> 213 µs per request:
  one query for the page's 20 entities instead of 20) - now faster than the same page by `offset` (6 914); corDB
  unchanged (21 576 / 21 380 - its retrieves by id were already one hash each).
- **`offset` itself is unchanged**: C sends MongoDB the same command for an `offset` page as A. The 100 000 store's
  `offset=500` on mongoc reads -11.2 % ± 10.8 % for C - MongoDB's CPU per request for it ranged over 1 284-1 852 µs
  across the runs and variants here, all sending that one command. That is the variance behind the "-21 %" above:
  with the profiler on (level 2) A and B send the identical `find` for the 300 000 store's `offset=1000` (filter
  `{type: {$in: [R1]}}`, sort `{createdAt: 1, _id: 1}`, skip 1 000, limit 21; plan `IXSCAN {type, createdAt, _id}`,
  1 021 keys, 21 documents, ~0.95 ms of CPU), and on ONE store they ran it at 3 644 and 3 645 requests/s (MongoDB CPU
  2 702 / 2 695 µs per request) - while across three stores built the same way the same query ran at 3 100-3 650.
- **The ids-only scan** (an automatic map with `--autoEntityMaps all`, or `entityMap=true`) reads the index alone:
  `explain()` of the 1 % type of 300 000 - `PROJECTION_COVERED` over `IXSCAN {type, createdAt, _id}`, 3 000 keys, 0
  documents. What it costs is the scan of every match, on every first page; an entity whose `type` is an array makes
  the index multikey, and then every match is read whole.
- **A map page's remote entities**, one request per Context Source: not measured (perfRun has no distributed
  scenario); `entitymap_page_per_source` counts the sources' requests - one entity query per source per page, where
  a page of 10 cost 6 + 3 entity retrieves before.

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
