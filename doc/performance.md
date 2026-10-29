# Performance and footprint

Every number on this page was measured on one machine, from release builds, and
each section says how. Re-measured on 2026-10-10 for 0.5.0, the broker's own code
profile-guided: the libmicrohttpd + `corDB` rows of the first throughput tables, the writes
table (both `corDB` on disk) and "Profile-guided". Re-measured on 2026-10-05, after corDB's log
became memory-mapped (a write's record is in the kernel before the response) and with the
databases on the host network: the eight-core section, what persistence costs, and the
libmicrohttpd + `mongoc` row. The rest of the per-core throughput, the container section
included, is from 2026-09-30 to 10-02, corDB in RAM; everything else (size, RAM, start-up, the `corHttp` rows, the client
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

<a id="size-on-disk--what-installing-coraine-adds"></a>

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
| `corHttp` + `corDB` | 1.66 MiB | 3.17 MiB (2) | **4.83 MiB** |
| `corHttp` + `mongoc` | 1.62 MiB | 9.94 MiB (5) | **11.56 MiB** |
| libmicrohttpd + `corDB` | 1.67 MiB | 10.89 MiB (11) | **12.56 MiB** |
| libmicrohttpd + `mongoc` | 1.63 MiB | 17.66 MiB (14) | **19.29 MiB** |

*(2026-10-21, main `f4f4a92a`, `coraine-build --release --bridges none` - `-O2`, every feature switch at
its default. Own code: the stripped `coraine`, the DB plugin and `none.so`. A bridge adds its plugin and
its libraries: [every choice `coraine-build` offers](building-details.md#every-choice-coraine-build-offers).)*

**A complete NGSI-LD broker, in-memory store included, is 4.8 MiB of things that
were not on the machine before — and the two libraries among them are GEOS.**

Those four rows all have TRoE off. Turning history on is the same measure, and
it is the clearest single number on this page:

| Configuration | coraine's own code | Added libraries | **Total added** |
|---------------|-------------------:|----------------:|----------------:|
| `corHttp` + `corDB` + **history in-process** | 1.65 MiB | 3.17 MiB (2) | **4.82 MiB** |
| `corHttp` + `corDB` + history in PostgreSQL | 1.73 MiB | 5.47 MiB (12) | 7.20 MiB |
| libmicrohttpd + `mongoc` + history in PostgreSQL | 1.70 MiB | 19.86 MiB (23) | **21.56 MiB** |

`--troe corDB` — temporal history in this process — adds **no library and no
measurable size**. The full conventional deployment is 23 libraries against 2,
and 21.6 MiB against 4.8.

What the three columns are made of:

- **coraine's own code is all of it.** The cor libraries are static archives,
  whole-archived into the binary, so that 1.5 MiB executable is not a `main` calling out to
  something else — it *is* (libmicrohttpd + `corDB`, from the linker map) the broker's own service
  routines (369 KiB), `corNgsild` (325 KiB of NGSI-LD rules), the broker's main program (148 KiB), the
  bridge layer (108 KiB), the exported-symbol tables that let plugins link against the broker (99 KiB),
  `corRest` (83 KiB), Service Execution (56 KiB), `corJsonld` (36 KiB), `corArgs` (26 KiB), `corJson`
  (22 KiB), `corHttp` (18 KiB - in both HTTP server builds, for the bridges' ports), the auth layer
  (18 KiB), `corTree` (17 KiB), `corBase` (14 KiB), and `corLog`, `corAlloc`, `corProm`, `corPlugin`,
  `corHash` under 5 KiB each. Every line of it is in this project's repositories. Add the DB plugin
  (`corDB.so` 189 KiB or `mongoc.so` 145 KiB) and `none.so` (14 KiB) and that is the whole broker.
- **The two added libraries in the first row** are GEOS (`libgeos` + `libgeos_c`, 3.17 MiB —
  geo-queries, loaded by the DB plugin). GEOS is the largest single thing coraine puts on a machine,
  and it is larger than coraine. `GEOQ=OFF` takes it away ([The smallest broker](#the-smallest-broker)).
- **libmicrohttpd costs nine libraries, 7.72 MiB** — not its own 1.02 MiB, because
  it pulls GnuTLS behind it, and p11-kit, nettle, hogweed, tasn1, idn2,
  libunistring, libffi. The built-in HTTP server removes all nine, and its build is no larger: `corHttp`
  is in both. That is the 2-vs-11 in the table.
- **`mongoc` costs three libraries, 6.77 MiB** — `libmongoc`, `libbson`,
  `libsasl2` — before MongoDB itself is installed anywhere.

### The smallest broker

Every feature switch off but `HEALTH`, `CHANNEL_STATUS_IN` and `CHANNEL_CHECKS` (`coraine-build --features
SUBSCRIPTIONS=OFF,REGISTRATIONS=OFF,...`). It keeps
the entity API - create, retrieve, query, update, merge, delete, the batch operations - the in-process
stores and the bridges with their Channels; subscriptions, registrations, geo-queries, Service Execution,
DataChannels and AssetChannels, auth, the policy API, cor://, the transports, SSE, metrics, the admin API,
Snapshot writes, `mongoc` and the rest are not compiled in.

| Configuration | coraine's own code | Added libraries | **Total added** |
|---------------|-------------------:|----------------:|----------------:|
| smallest, `corHttp` + `corDB` | 1.35 MiB | **none** | **1.35 MiB** |
| smallest, `corHttp` + `ramDB` | 1.33 MiB | none | 1.33 MiB |
| smallest, libmicrohttpd + `corDB` | 1.36 MiB | 7.72 MiB (9) | 9.08 MiB |

| The executable, stripped | every switch at its default | smallest | |
|---|---:|---:|---:|
| `corHttp` | 1 533 312 bytes | 1 220 224 bytes | **-20.4 %** |
| libmicrohttpd | 1 546 176 bytes | 1 224 320 bytes | -20.8 % |

*(2026-10-21, main `f4f4a92a`, `coraine-build --release --bridges none`, the same as the table above.)*

- **The switches take a fifth of the executable; GEOS is the rest of the saving.** 4.83 MiB to 1.35 MiB
  is 3.17 MiB of GEOS (`GEOQ=OFF`) and 0.31 MiB of coraine's own code.
- **What goes, from the linker map:** Service Execution (56 KiB), the service routines 369 → 293 KiB,
  the main program 148 → 107 KiB, the bridge layer 108 → 60 KiB (DataChannels, AssetChannels, the
  inventory), `corRest` 83 → 65 KiB, the auth layer (18 KiB), the transports (13 KiB), the policy API
  (8 KiB), SSE (7 KiB), metrics (5 KiB).
- **What stays is mostly NGSI-LD itself.** `corNgsild` (325 KiB), the rules every entity request is
  checked against, is the same in both - over a quarter of the smallest executable.
- **A bridge is added by plugin**: the Modbus bridge 31 KiB, no library
  ([every choice `coraine-build` offers](building-details.md#every-choice-coraine-build-offers)).

> **ICU is off in every row above, deliberately.** § 7.6.2.1 names ICU "root"
> collation as the default order for `orderBy` on strings, and linking ICU for it
> costs **three libraries and 39.2 MiB** — `libicudata` alone is 31.6 MiB, six
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

### The stored trees

What `corDB` keeps of an entity is a tree of `CorNode`s, and the subscription, registration and
snapshot caches are trees too. They are allocated in the tree heap ([corTree](https://github.com/SEAMWARE/corTree)'s `corTreeHeap.c`): a
node, its name and a string value are one block, 8-byte aligned, carved from 16 KiB pages of one
size each - no allocator header per block and no rounding to 16 bytes, which with `malloc` were 8
to 23 bytes of every block. A node that is not an object or an array is 32 bytes there, not 40: an
object's last child is the node's last 8 bytes ([the CorNode
layout](plugin-architecture.md#the-cornode-layout)), and only a container has one.

100 000 entities of perfRun's larger store (five attributes, 24 nodes each: 13 containers, 6
strings, 3 integers, 2 floats), `corDB` in RAM, release builds, loaded in batches of 500, the
broker's RSS after the load - two runs of each, in the order A B B A, within 0.2 % (2026-10-23):

| Build | RSS | Per entity | |
|---|---:|---:|---:|
| `malloc`, 40-byte nodes (0.5.x) | 270 MiB | 2 445 B | |
| tree heap, 40-byte nodes | 240 MiB | 2 140 B | −11 % |
| tree heap, 32-byte scalars (0.6.0) | 231 MiB | 2 047 B | −14.5 % |

The load took the same time in all three (23.5 - 23.7 s). The nodes outside these trees - a
request's, in its arena - stay 40 bytes: they are thrown away with the request, and code that
turns a node into another kind in place does so on them.

`COR_TREE_HEAP=malloc` puts the trees back in `malloc`, a block per node - for valgrind, which sees
a block's bounds only in `malloc`'s (`corTest -vt` sets it). A build with AddressSanitizer does
the same by itself.

### Threads building trees at once

A tree's nodes come in a few sizes, and every thread writing the store builds and frees them: a batch
update of 20 entities clones and frees thousands. Each thread keeps a cache of free blocks per size
class, taken and freed with no lock; the class's mutex is taken once for a batch of blocks (4 KiB worth,
32 at most) - from the class's free list or, else, a range of its page, whose memory is first touched
outside the mutex - and once to give a batch back when a thread holds two. A thread that ends gives its
cache back. The cost is up to 8 KiB a class a thread; with 300 000 entities stored the broker's RSS did
not move (641 MB both).

`test/perf/perfRun.sh corDB`, the durable configuration (`--dbDir` on ext4, NVMe, `--dbSync interval`,
`--troe none`), plain release, AMD Ryzen 9 8940HX, coraine `2fa6186e` + corDB `173c7c7` with corTree's
heap before and after (corTree `3777629`), two runs of each in the order A B B A, 2026-10-27 -
requests/s:

| scenario | one mutex a class | thread caches | change |
|---|---:|---:|---:|
| batch update (20), c50 | 16 269 | 29 076 | **+78.7 %** |
| batch create (20), c50 | 10 474 | 29 981 | **+186 %** |
| batch delete (20), c50 | 18 590 | 60 452 | **+225 %** |
| merge, c50 | 65 497 | 70 702 | +7.9 % |
| `PATCH`, c50 | 100 513 | 104 067 | +3.5 % |
| create, c50 | 90 415 | 93 566 | +3.5 % |
| create into 300 000 entities, c50 | 87 680 | 88 423 | +0.8 % |
| `DELETE`, c50 | 150 654 | 147 052 | −2.4 % |
| `PATCH`, c1 | 46 510 | 47 226 | +1.5 % |
| create, c1 | 33 841 | 33 909 | +0.2 % |

Reads and subscriptions: within ±3.3 %. The batch p50s halve or better (batch update 2.92 → 1.53 ms,
batch delete 2.53 → 0.73 ms). With one connection a batch did the same work either way; what the cache
removes is threads waiting for each other on the same few sizes.

## What else has to be running

| `--database` / `--troe` | Also required | What that costs |
|-------------------------|---------------|-----------------|
| `corDB` / `none` | **nothing** | — |
| `corDB` / `corDB` | **nothing** | temporal history in the same process, and it is free — see below |
| `mongoc` | a MongoDB server | 1.02–1.15 GiB resident at rest, and WiredTiger's cache defaults to half of (RAM − 1 GiB): on this 60 GiB host mongod is entitled to ~30 GiB. Plus **cores** — see below. Image: `mongo:4.4` 594 MB, `mongo:8` 1.3 GB |
| `timescale` (TRoE) | a PostgreSQL + TimescaleDB server | 35 MB of packages, 128 MiB of shared buffers by default, 313 MiB across its 10 processes here — and on the broker's own machine **2.3 MiB and 9–10 added libraries** for `libpq`, which drags in Kerberos, LDAP and SASL that a broker never calls. Nine rather than ten when `mongoc` has already brought `libsasl2` |

Which is the point of the first two rows. A `corHttp` + `corDB` deployment with
`--troe corDB` is **one process, 17 MiB, 4.8 MiB of new files on disk, and no socket
to anything else** — with temporal history included.

## Start-up

From `exec` to a served HTTP response:

| Configuration | Ready in |
|---------------|---------:|
| libmicrohttpd + `corDB` | **9 ms** |
| `corHttp` + `corDB` | 12 ms |
| libmicrohttpd + `mongoc` | 26 ms |
| `corHttp` + `mongoc` | 31 ms |

[corHttp](https://github.com/SEAMWARE/corHttp)'s extra 3 ms is the connection pool it allocates up front; mongoc's
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
| 1 entity | **82 672** | 82 672 |
| 20 entities | 12 577 | **251 540** |
| 100 entities | 2 750 | **275 000** |

*(libmicrohttpd + `corDB` on disk (`--dbDir`, the durable configuration), PGO release with the broker's
own code profile-guided, 2026-10-10, coraine 0.5.0 - the conditions in "Profile-guided"; in RAM the
same build within 2 %. The per-request cost is fixed, so the bigger the page the more of it is
amortised — and the entities/s column is still climbing at 100.)*

**All four builds, `limit=20`:**

| Configuration | req/s per core | entities/s per core |
|---------------|---------------:|--------------------:|
| libmicrohttpd + `corDB` | **12 577** | **251 540** |
| `corHttp` + `corDB` | 9 508 | 190 160 |
| libmicrohttpd + `mongoc` | 7 342 | 146 840 |
| `corHttp` + `mongoc` | 4 634 | 92 680 |

*(libmicrohttpd + `corDB` 2026-10-10, PGO release, `corDB` on disk (`--dbDir`). libmicrohttpd +
`mongoc` 2026-10-05, PGO release (the libraries only, before #307), against MongoDB 8.2 on the host
network, pinned to seven other cores. `corHttp` rows 2026-09-16, without PGO; its `mongoc` row against
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
without PGO, [corTools](https://github.com/SEAMWARE/corTools)
`corRequest --rate`, latency counted from when a request was due) and 16 connections, HTTP all the
way: small entity at 20 000 req/s p50 174-176 µs, p99 0.23-1.85 ms; at 25 000, p99 0.27-0.81 ms;
20 attributes at 15 000, p50 310-377 µs, p99 0.55-0.65 ms. Past ~30 000 (small) and ~20 000 (20
attributes) the p99 goes to 12-24 ms.

### Writes, and what batching is worth

**Per core, `corDB`:**

| Operation | req/s | **entities/s** | vs one at a time |
|---|---:|---:|---:|
| `PATCH` one attribute, 50 clients | 104 400 | 104 400 | — |
| `PATCH`, 1 client | 48 625 | 48 625 | — |
| batch update, 20 per request | 10 022 | **200 440** | **1.9×** |
| merge (`PATCH /entities/{id}`), 50 clients | 78 548 | 78 548 | — |
| create one entity | 54 707 | 54 707 | — |
| batch create, 20 per request | 5 873 | **117 460** | **2.1×** |
| delete one entity | 126 714 | 126 714 | — |
| batch delete, 20 per request | 23 814 | **476 280** | **3.8×** |

*(2026-10-10, libmicrohttpd, `corDB` on disk (`--dbDir`, the durable configuration), PGO release with
the broker's own code profile-guided - the conditions in "Profile-guided", which also has the same
build in RAM. Deletes are measured against a store of 40 000 entities, refilled before every repeat -
a delete consumes what it measures.)*

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
it pays. Every library is in it, and the broker's own code: since #307 (2026-10-09) the profile flags
reach the broker's sources too (`COR_C_FLAGS_RELEASE`); before it CMake's `CMAKE_C_FLAGS_RELEASE`
replaced them and only the libraries were profile-guided.

**2026-10-09/10, the broker's own code profile-guided since #307.** Against the same source and flags
without the profile:

| | |
|---|---|
| Source | coraine `e289dc6c` (`main`); [corBase](https://github.com/SEAMWARE/corBase) `d6fcc0a`, [corLog](https://github.com/SEAMWARE/corLog) `339e2f3`, [corAlloc](https://github.com/SEAMWARE/corAlloc) `c328237`, [corArgs](https://github.com/SEAMWARE/corArgs) `48e963b`, [corHash](https://github.com/SEAMWARE/corHash) `d25d65b`, corTree `9d63d52`, [corJson](https://github.com/SEAMWARE/corJson) `bd0c7b4`, [corProm](https://github.com/SEAMWARE/corProm) `f20d9ab`, corHttp `cfc49f2`, [corBridge](https://github.com/SEAMWARE/corBridge) `1a57ea7`, [corPlugin](https://github.com/SEAMWARE/corPlugin) `86d0726`, [corRest](https://github.com/SEAMWARE/corRest) `27944dc`, [corJsonld](https://github.com/SEAMWARE/corJsonld) `c7bfcde`, [corNgsild](https://github.com/SEAMWARE/corNgsild) `5cfed34`, corDB `9ae7c0d` - every one at its `main` |
| Builds | **without**: `make release`; **PGO**: `make pgo PGO_RESTORE_DEBUG=0` (as the Docker image), trained by `pgoTrain.sh` with every step completing. Each with `COR_HTTP_SERVER=builtin` and with libmicrohttpd (the default, as the Docker image and the packages); every library release, the libraries rebuilt from clean for each `make release`. The broker's 21 targets compiled with `-fprofile-use`, 154 profile files (`.gcda`) for the broker's own sources, `.text.hot` sections in 25 of its objects (none in the build without) |
| Machine | AMD Ryzen 9 8940HX (16 cores / 32 threads), 60 GiB, Linux 7.0, glibc 2.43, gcc 15.2.0; governor `powersave` (amd-pstate-epp, `balance_performance`), boost on, idle states **deep** - as the machine ships (not changed: it needs root) |
| Load | `test/perf/perfRun.sh corDB`, `PERF_BROKER_CORES=1` (the broker on CPU 0, wrk on CPUs 1-15), 5 s x 3 per scenario (perfRun's defaults), a run's figure the median of its three; `--httpLoops 1` on the built-in server; the delete scenarios' wrk ceiling cut from 60 s to 3 s as in "The request arena's sizes" (the rate is `Consumed/sec`, the same either way); perfRun's rare-type and larger-store scenarios with `PERF_RARE_ENTITIES=100 PERF_BIG_ENTITIES=1000` and not reported |
| Store | `corDB` in RAM (no `--dbDir`), as the table of 2026-10-02 below; the libmicrohttpd PGO build also with `--dbDir` on the local NVMe disk (ext4 on LUKS), `--dbSync interval` (the default) - the durable configuration |
| Runs | three per build, interleaved (without, PGO, without, ...): built-in server 2026-10-09 22:50-23:38, libmicrohttpd 23:38-00:25, libmicrohttpd PGO `--dbDir` 00:25-00:50; every run started with no browser running, no other broker, test or load generator, and the load average below 1 |

Requests/s: the median of the three runs, ± half the spread of the three:

| One core, built-in server, `corDB` in RAM | without | **PGO** | | p99 ms, without / PGO |
|---|---:|---:|---:|---:|
| query, 1 entity | 75 408 (±1.3 %) | **80 457** (±1.5 %) | +6.7 % | 0.71 / 0.66 |
| query, 20 entities | 11 597 (±1.6 %) | **12 310** (±1.2 %) | +6.1 % | 4.22 / 3.95 |
| query, 100 entities | 2 510 (±1.3 %) | **2 737** (±0.9 %) | +9.0 % | 19.27 / 17.65 |
| query, 20 entities, 200 connections | 11 580 (±1.5 %) | **12 350** (±0.9 %) | +6.6 % | 17.70 / 16.48 |
| retrieve | 84 276 (±0.7 %) | **88 391** (±1.2 %) | +4.9 % | 0.65 / 0.62 |
| PATCH, 1 connection | 45 263 (±5.0 %) | **50 054** (±4.2 %) | +10.6 % | 0.03 / 0.03 |
| PATCH, 50 connections | 99 371 (±0.9 %) | **105 918** (±0.6 %) | +6.6 % | 0.55 / 0.52 |
| create | 64 129 (±0.5 %) | **68 981** (±1.2 %) | +7.6 % | 0.87 / 0.81 |
| create, 1 connection | 36 451 (±2.3 %) | **38 320** (±0.9 %) | +5.1 % | 0.04 / 0.04 |
| merge | 76 901 (±0.9 %) | **80 386** (±0.8 %) | +4.5 % | 0.69 / 0.66 |
| delete | 110 834 (±2.6 %) | **116 330** (±2.9 %) | +5.0 % | 0.48 / 0.48 |
| batch update (20) | 12 445 (±2.0 %) | **13 649** (±1.4 %) | +9.7 % | 4.01 / 3.58 |
| batch create (20) | 8 884 (±1.1 %) | **9 801** (±1.7 %) | +10.3 % | 5.59 / 5.12 |
| batch delete (20) | 29 635 (±1.1 %) | **30 399** (±1.2 %) | +2.6 % | 1.70 / 1.65 |
| PATCH, 1 subscriber | 40 046 (±1.6 %) | **43 168** (±3.4 %) | +7.8 % | 1.36 / 1.25 |
| PATCH, ~210 subscriptions | 37 188 (±1.8 %) | **39 599** (±3.4 %) | +6.5 % | 1.40 / 1.31 |

| One core, libmicrohttpd, `corDB` | without, in RAM | **PGO**, in RAM | | **PGO**, `--dbDir` | `--dbDir` vs in RAM | p99 ms, without / PGO / PGO `--dbDir` |
|---|---:|---:|---:|---:|---:|---:|
| query, 1 entity | 77 951 (±0.5 %) | **81 083** (±0.9 %) | +4.0 % | **82 672** (±0.6 %) | +2.0 % | 0.69 / 0.65 / 0.66 |
| query, 20 entities | 11 596 (±0.4 %) | **12 536** (±0.5 %) | +8.1 % | **12 577** (±1.0 %) | +0.3 % | 4.20 / 3.88 / 3.91 |
| query, 100 entities | 2 540 (±0.4 %) | **2 742** (±9.4 %) | +8.0 % | **2 750** (±9.9 %) | +0.3 % | 19.01 / 18.00 / 17.56 |
| query, 20 entities, 200 connections | 11 544 (±0.3 %) | **12 477** (±0.8 %) | +8.1 % | **12 477** (±0.7 %) | +0.0 % | 19.01 / 16.50 / 16.54 |
| retrieve | 87 834 (±1.2 %) | **91 141** (±0.3 %) | +3.8 % | **90 916** (±0.4 %) | -0.2 % | 0.60 / 0.58 / 0.58 |
| PATCH, 1 connection | 50 279 (±1.7 %) | **50 243** (±2.3 %) | -0.1 % | **48 625** (±2.6 %) | -3.2 % | 0.03 / 0.03 / 0.76 |
| PATCH, 50 connections | 109 111 (±0.6 %) | **115 173** (±0.2 %) | +5.6 % | **104 400** (±0.5 %) | -9.4 % | 0.52 / 0.48 / 0.54 |
| create | 66 137 (±0.1 %) | **69 936** (±1.0 %) | +5.7 % | **54 707** (±0.9 %) | -21.8 % | 0.82 / 0.77 / 4.37 |
| create, 1 connection | 35 353 (±2.4 %) | **38 692** (±1.7 %) | +9.4 % | **34 032** (±1.6 %) | -12.0 % | 0.04 / 0.03 / 0.87 |
| merge | 80 911 (±0.8 %) | **84 160** (±1.5 %) | +4.0 % | **78 548** (±0.7 %) | -6.7 % | 0.65 / 0.63 / 0.69 |
| delete | 127 055 (±1.0 %) | **128 309** (±1.0 %) | +1.0 % | **126 714** (±2.4 %) | -1.2 % | 0.44 / 0.43 / 0.44 |
| batch update (20) | 11 250 (±2.0 %) | **12 038** (±0.9 %) | +7.0 % | **10 022** (±1.0 %) | -16.7 % | 4.46 / 4.22 / 5.45 |
| batch create (20) | 7 869 (±0.5 %) | **8 470** (±0.6 %) | +7.6 % | **5 873** (±0.8 %) | -30.7 % | 6.34 / 5.99 / 21.61 |
| batch delete (20) | 24 850 (±0.6 %) | **25 325** (±0.9 %) | +1.9 % | **23 814** (±0.2 %) | -6.0 % | 3.06 / 2.93 / 3.26 |
| PATCH, 1 subscriber | 21 868 (±1.3 %) | **22 670** (±1.7 %) | +3.7 % | **22 043** (±0.9 %) | -2.8 % | 2.72 / 2.28 / 2.97 |
| PATCH, ~210 subscriptions | 20 800 (±2.6 %) | **21 741** (±0.9 %) | +4.5 % | **21 416** (±1.7 %) | -1.5 % | 2.52 / 2.39 / 2.66 |

- **PGO, built-in server: +2.6 % (batch delete) to +10.6 % (`PATCH`, one connection)**, every scenario.
  The three runs of the two builds do not overlap on any scenario but single-connection `PATCH`
  (45 263 / 46 217 / 41 653 against 47 029 / 50 054 / 51 263).
- **PGO, libmicrohttpd: +1.0 % (delete) to +9.4 % (create, one connection)**; single-connection `PATCH`
  -0.1 %, within its spread.
- **Query, 100 entities, libmicrohttpd PGO**: one of the three runs of each store low (2 245 in RAM,
  2 216 on disk; the other two 2 742-2 762), which is the ±9.4 % and ±9.9 %; without it the spread
  is ±0.3 %.
- **Against the 2026-10-02 table below** (another day, other sources): the builds without the profile
  within -5.3 % (query, 1 entity) to +3.5 % (create); PGO lower on reads (query, 20 entities 12 310
  against 12 702; retrieve 88 391 against 90 200) and higher on writes (`PATCH`, 50 connections 105 918
  against 100 851; create 68 981 against 64 417).
- **`--dbDir` against RAM, the same PGO build: reads within -0.2 % to +2.0 %; writes -1.2 % (delete) to
  -30.7 % (batch create)**, the store growing and snapshotting the most there ("corDB on disk: what
  persistence costs").

**2026-10-02, the libraries only** (before #307 the broker's own code was compiled without the profile -
the flags did not reach it). One core, `corDB` in RAM, built-in server:

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

2026-10-02, the libraries only: user-space instructions per request: retrieve -4.5 %, PATCH -3.5 %, a 20-entity query -6.4 %. The
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
| create | 43 811 | 25 605 | 117 299 | 83 325 | 201 907 | 6 508 | 5 009 |
| create, 1 connection | 8 181 | 8 112 | 28 540 | 22 816 | 31 407 | 4 203 | 1 043 |
| batch create (20) | 12 070 | 2 463 | 13 864 | 7 955 | 51 141 | 3 610 | 1 804 |
| merge | 27 150 | 19 195 | 101 528 | 80 441 | 124 902 | 6 437 | 5 308 |
| `PATCH` | 17 486 | 13 215 | 145 664 | 120 379 | 174 382 | 6 519 | 5 497 |
| `PATCH`, 1 connection | 3 891 | 3 424 | 38 332 | 36 714 | 39 631 | 3 504 | 964 |
| batch update (20) | 3 130 | 2 227 | 34 762 | 23 239 | 40 501 | 2 472 | 2 120 |
| `DELETE` | 28 399 | 23 547 | 205 280 | 183 448 | 221 244 | 18 912 | 9 343 |
| batch delete (20) | 7 351 | 6 278 | 67 070 | 48 329 | 69 480 | 3 163 | 2 467 |
| `GET /entities/{id}` | 55 595 | 51 998 | 481 812 | 493 889 | 469 348 | 31 727 | 31 441 |
| query, `limit=20` | 24 143 | 22 957 | 76 649 | 78 933 | 76 213 | 5 543 | 5 507 |

**Latency, p50 / p95 / p99, ms:**

| | coraine + MongoDB | coraine + MongoDB + TimescaleDB | coraine, corDB on disk | coraine, corDB on disk + history | coraine, ramDB | Orion-LD + MongoDB | Orion-LD + MongoDB + PostgreSQL |
|---|---:|---:|---:|---:|---:|---:|---:|
| create | 1.08 / 1.44 / 2.37 | 1.59 / 13.9 / 28.5 | 0.32 / 8.38 / 15.6 | 0.42 / 16.4 / 35.9 | 0.23 / 0.37 / 0.45 | 6.86 / 14.6 / 19.3 | 9.41 / 18.3 / 25.4 |
| create, 1 connection | 0.11 / 0.18 / 0.20 | 0.11 / 0.15 / 0.17 | 0.03 / 0.04 / 1.02 | 0.04 / 0.06 / 1.32 | 0.03 / 0.03 / 0.04 | 0.23 / 0.28 / 0.34 | 0.92 / 1.13 / 1.21 |
| batch create (20) | 3.60 / 9.55 / 27.3 | 18.4 / 68.3 / 109 | 2.35 / 10.1 / 12.5 | 4.09 / 24.8 / 78.6 | 0.83 / 1.92 / 2.85 | 12.4 / 27.9 / 38.3 | 26.2 / 46.6 / 61.8 |
| merge | 1.76 / 2.15 / 2.50 | 2.44 / 4.10 / 4.78 | 0.46 / 0.86 / 1.06 | 0.54 / 1.15 / 1.43 | 0.38 / 0.63 / 0.76 | 7.35 / 11.9 / 15.2 | 8.95 / 17.0 / 22.9 |
| `PATCH` | 2.73 / 3.20 / 3.48 | 3.56 / 5.98 / 6.70 | 0.32 / 0.56 / 0.69 | 0.36 / 0.76 / 0.94 | 0.26 / 0.45 / 0.54 | 7.25 / 11.8 / 14.7 | 8.63 / 16.6 / 22.8 |
| `PATCH`, 1 connection | 0.24 / 0.29 / 0.31 | 0.28 / 0.33 / 0.37 | 0.02 / 0.03 / 0.03 | 0.03 / 0.03 / 0.03 | 0.02 / 0.02 / 0.03 | 0.28 / 0.29 / 0.32 | 1.04 / 1.09 / 1.19 |
| batch update (20) | 15.3 / 17.8 / 19.1 | 21.5 / 28.9 / 32.8 | 1.32 / 1.83 / 2.51 | 1.95 / 2.97 / 3.97 | 1.17 / 1.37 / 1.50 | 18.6 / 31.4 / 39.3 | 21.7 / 40.9 / 53.8 |
| `DELETE` | 1.65 / 1.98 / 2.49 | 1.94 / 3.42 / 4.33 | 0.19 / 0.37 / 0.45 | 0.23 / 0.40 / 0.47 | 0.17 / 0.37 / 0.46 | 2.46 / 3.91 / 5.37 | 5.09 / 8.93 / 12.0 |
| batch delete (20) | 5.71 / 13.3 / 15.9 | 6.93 / 14.8 / 19.2 | 0.66 / 0.86 / 0.96 | 0.92 / 1.34 / 1.49 | 0.57 / 0.80 / 1.18 | 13.7 / 27.0 / 35.9 | 16.5 / 38.1 / 75.2 |
| `GET /entities/{id}` | 0.86 / 1.11 / 1.33 | 0.92 / 1.21 / 1.48 | 0.10 / 0.17 / 0.18 | 0.09 / 0.13 / 0.14 | 0.10 / 0.16 / 0.17 | 1.47 / 2.79 / 4.11 | 1.48 / 2.84 / 4.18 |
| query, `limit=20` | 1.98 / 2.76 / 3.36 | 2.07 / 3.00 / 3.66 | 0.63 / 1.16 / 1.17 | 0.54 / 0.93 / 0.94 | 0.60 / 1.06 / 1.08 | 7.94 / 17.2 / 21.7 | 8.01 / 17.5 / 22.4 |

Eight physical cores for the whole deployment - the broker, and the databases it needs (mongod 8.2 and
PostgreSQL 16 + TimescaleDB as containers on the host network - no port mapping - pinned to the same
cores); the load generator on the other eight, standing in for clients. 50 connections unless said,
median of 3 × 5 s, `test/perf/perfRun.sh`, AMD Ryzen 9 8940HX, 2026-10-05 - the column coraine +
MongoDB + TimescaleDB 2026-10-21, its history written by the writers (`--troeWriters`, 8); the reads,
which no history touches, came out 3-6 % apart across the two dates. coraine a PGO release,
corDB on disk with `--dbDir`, its log memory-mapped: a write's record is in the kernel before the
response, so a broker that dies loses nothing it acknowledged, and a machine that dies at most the
last 100 ms - MongoDB's default. Orion-LD 1.15.0-next, its release build (`-O3`), `-mongocOnly`.

- **History is where it shows.** In a database server it costs a write 1-80 % (coraine + MongoDB +
  TimescaleDB: create 43 811 → 25 605, batch create (20) 12 070 → 2 463; Orion-LD + PostgreSQL 14-75 %);
  in corDB 4-43 %, and coraine with corDB and its history outruns MongoDB without any on every write but
  batch create (20) - and MongoDB + TimescaleDB 2.8-11×.
- **Against Orion-LD with its history**: coraine with corDB and its history is 4.4-38× on every row,
  reads included.
- **Persistence costs little:** ramDB, no disk at all, is 1.0-1.7× corDB on disk; 3.7× on batch
  create, which grows the store fastest and so snapshots most.
- **Reads** do not touch history in any configuration. corDB answers a retrieve 8.7× MongoDB's rate,
  481 812 a second at a p99 of 0.18 ms.
- **The tails of a write** follow the throughput: p99 of a batch update 3.97 ms (corDB + history) against
  32.8 ms (MongoDB + TimescaleDB).

### TimescaleDB: the write path

The history of a write as the timescale plugin writes it ([TimescaleDB history](installation.md#timescaledb-history---troe-timescale)),
the same release broker, only `timescale.so` swapped. Requests/s **counted until the last history row
is in the database**: the run's requests over the time from its start until `troe_attrs` stopped
growing for a second - a write that is queued is not done.

| | before (a statement per row) | now | |
|---|---:|---:|---:|
| create | 4 446 | 24 025 | 5.4× |
| create, 1 connection | 710 | 7 601 | 10.7× |
| batch create (20) | 372 | 2 321 | 6.2× |
| create, `--troeSync` | 4 336 | 12 725 | 2.9× |
| create, 1 connection, `--troeSync` | 682 | 994 | 1.5× |
| batch create (20), `--troeSync` | 248 | 813 | 3.3× |

Where the cores went, create, 50 connections (eight shared): before - broker 1.0, mongod 0.5,
PostgreSQL 4.6, 22 300 history rows/s; now - broker 2.3, mongod 2.6, PostgreSQL 2.5, 120 000 rows/s.
The eight cores are spent: the broker without history, on the same eight, creates 43 666/s on 3.6
cores (82 µs a create; 97 µs with the history). The `--troeSync` rows of this section are a
`--troeSync` whose response waited for the commit, for each event it recorded: a batch create of 20,
20 in a row. It no longer waits ([below](#--troesync-handed-over-before-the-response)).

Step by step - each an A/B against the step before, ABBA, the mean of the two runs of each:

| Step | create | create, 1 conn. | batch create (20) |
|---|---:|---:|---:|
| a statement per row (before) | 4 427 | 715 | 370 |
| the rows of a request in one `INSERT` per table | 6 152 | 987 | 1 056 |
| writers (8): the rows of many requests in one transaction | 17 765 | 6 020 | 1 172 |
| a column an array, unnested, instead of a `VALUES` list | 23 343 (17 853) | 5 872 (6 110) | 1 691 (1 242) |
| a waiting `--troeSync` request woken by its own condition | - | - | - |
| another writer only when none writes, or 8 requests wait | 23 003 (22 553) | 7 289 (5 875) | 1 715 (1 706) |
| a batch up to 1 MiB (was 4) | 22 939 (22 646) | 7 284 (7 271) | 1 837 (1 706) |
| the optional indexes off by default (`--troeSqlIndexes none`) | see the first table | | |
| a row's timestamps from integers, not `%.6f` | 24 670 (24 782) | 7 449 (7 437) | 2 170 (2 237) |

(in parentheses: the step before, in the same ABBA - runs differ by a few per cent from one to the
next.) The waiting request's own condition is a `--troeSync` step: create 10 892 → 11 306, batch
create 712 → 760 - and the broker's cores on that batch create 1.10 → 0.65. With `--troeSync`, the
writers against the request's own transaction: create 6 094 → 10 356, 1 connection 956 → 1 046,
batch create 341 → 671; unnesting: 10 429 → 10 742, 1 029 → 951, 685 → 709; the writer wake-up:
11 103 → 11 777, 974 → 962, 754 → 759.

**The broker's share:** the history work after a create's response was 99 960 instructions
(callgrind, c1), more than half of it glibc's multi-precision float printer on the two epoch-second
timestamps of each row; from integers, 47 450. The requests/s did not move - the broker's cores on
batch create did, 0.88 → 0.75. A create costs the broker 82 µs without history, 97 µs with.

**Unnesting:** one `INSERT` of 1000 rows into `troe_attrs` took 55 ms as a `VALUES` list - 25
executing, 9 planning, the rest parsing its 16 000 expressions - and 22 ms as `INSERT ... SELECT
... FROM unnest(<an array per column>)`; prepared, the same 22.

### `--troeSync`: handed over before the response

`--troeSync` hands a request's history to the writers once its service routine is done, before the
response, instead of after it; the response does not wait for the commit. Against the default, one
PGO build, eight cores shared by the broker, mongod 8.2 and PostgreSQL 16 + TimescaleDB, `perfRun.sh`,
ABBA - A the default, B handed over before the response:

| requests/s | A | A | B | B | A, run to run | B against A |
|---|---:|---:|---:|---:|---:|---:|
| create | 25 657 | 26 132 | 25 291 | 25 772 | 1.8 % | -1.4 % |
| create, 1 connection | 8 003 | 8 089 | 7 488 | 7 570 | 1.1 % | -6.4 % |
| batch create (20) | 2 406 | 2 393 | 2 383 | 2 363 | 0.5 % | -1.1 % |
| merge | 19 163 | 19 364 | 18 944 | 18 901 | 1.0 % | -1.8 % |
| `PATCH` | 13 273 | 12 969 | 12 843 | 12 832 | 2.3 % | -2.2 % |
| `PATCH`, 1 connection | 3 446 | 3 444 | 3 353 | 3 360 | 0.1 % | -2.6 % |
| batch update (20) | 2 217 | 2 077 | 2 058 | 2 052 | 6.5 % | -4.3 % |
| `DELETE` | 23 725 | 24 362 | 23 710 | 22 905 | 2.6 % | -3.1 % |
| batch delete (20) | 6 224 | 6 241 | 6 178 | 6 132 | 0.3 % | -1.2 % |
| `PATCH`, subscriptions | 12 936 | 12 914 | 12 938 | 12 902 | 0.2 % | 0.0 % |
| `GET /entities/{id}` | 52 268 | 52 203 | 52 045 | 52 045 | 0.1 % | -0.4 % |

With one connection the work after the response overlaps the client's round trip; before it, it is
added to each request's latency - about 6 µs a create. With 50 connections the cores are spent
either way. The same writes with the commit waited for (the nightly run of 2026-10-21, the same
deployment): create 13 558, create on 1 connection 958, batch create 876, `PATCH` 12 028, `PATCH` on
1 connection 825, batch update 611, batch delete 1 572.

A fifth run, a second B, is left out: it ran on battery, the lid closed - 15-20 % lower on every
test, the reads too. The B in the table is the one run again on mains.

**The number of writers** (before unnesting, one run each; the queue up to 64 MiB then, 8 since):

| `--troeWriters` | 2 | 4 | 8 | 16 |
|---|---:|---:|---:|---:|
| create | 10 029 | 14 260 | 17 244 | 16 335 |
| create, 1 connection | 7 487 | 6 680 | 5 891 | 5 927 |
| batch create (20) | 531 | 850 | 1 223 | 1 094 |

With the 1 MiB batches, batch create (20): 4 writers 1 639, 8 1 892, 16 1 543.

**The indexes** (each index updated for every row). Dropped one after another, writers, a `VALUES`
list: batch create (20) 1 221 with all; without `time` and `troe_entities_id_modified`, the same;
then without `attrObserved` +10 %, without `geo` +11 %, without (`entity_id`, `attr_name`,
`observed_at`) +5 % - one of the basic ones, kept. After unnesting, batch create (20) / create:
all 1 671 / 23 092; without the `entity_type` GINs 1 898 / 23 073; without `time`, `attrObserved`,
`geo` and (`entity_id`, `attr_name`, `observed_at`) 2 252 / 25 906; without those and the GINs
2 545 / 26 850. What each serves, and what a query costs without it:
[the indexes of the history](installation.md#the-indexes-of-the-history).

Not the write path: PostgreSQL wrote 7.5 WAL records and 1 415 bytes per history row, 0.01 full-page
images - no checkpoint pressure.

Eight physical cores shared by the broker, mongod 8.2 and PostgreSQL 16 + TimescaleDB (containers, host
network, `synchronous_commit` on - every commit on the disk); the load (wrk) on the other eight. 50
connections unless said, 5 s after a 2 s warm-up, both databases emptied before each measure; AMD
Ryzen 9 8940HX, 2026-10-21.

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
- **A store that pauses** when its log cannot be written ([the pause](installation.md#a-store-that-cannot-write-its-log))
  costs a write one atomic load: every single-entity scenario within ±2.5 % of the same broker without
  it (2026-10-27, the conditions of "Threads building trees at once").
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

### The q pre-filter and declared indexes on mongoc

A `q` value term on `mongoc` is a `$expr` over every instance of the attribute, and no index serves it. The
pre-filter (`COR_FEATURE_Q_PREFILTER`) adds a condition in front of it that an index can serve
([q and the mongoc indexes](installation.md#q-and-the-mongoc-indexes)). A declared index provides that index
([Declared indexes](installation.md#declared-indexes)).

**Reads.** Measured in mongosh, 2026-10-22, MongoDB 8.2.12: 200 000 documents in the shape mongoc writes, the
filter mongoc builds for `q=speed>=995`, 1 200 matches:

| filter | time |
|---|---:|
| `$expr` only (before the pre-filter) | 258.8 ms |
| pre-filter + `$expr`, no index | 73.4 ms |
| pre-filter + `$expr`, the two indexes of a declaration | 3.4 ms - 1 200 documents examined |

The broker's own query, read from MongoDB's profiler, with `--dbValueIndexes` and `/admin/indexes`
(`db_indexes_mongoc.test`): `type=T&q=temp>190` over 201 entities uses both indexes of `temp` and examines the
6 entities it returns. A query whose type a declaration narrowed to another type does not use them.

**Writes: what the mark costs.** Every write of an attribute checks whether it has an instance of a datasetId,
to write the mark. A/B, 2026-10-22 10:43-11:47: release build of `main` `3197d718`; A = its `mongoc.so`, B =
`mongoc.so` from the same source with `-DCOR_FEATURE_Q_PREFILTER=OFF`, only the plugin swapped between runs. perfRun
with its defaults (5 s x 3 repeats, 8 threads, no pinning), MongoDB 8.2.12 local, the perf database dropped
before each run. Order ON, OFF, OFF, ON; the figure is the mean of the two runs of each:

| scenario | ON / OFF |
|---|---:|
| create, 50 connections | +1.0 % |
| create, 1 connection | -0.6 % |
| batch create of 20, 50 connections | +0.1 % |
| PATCH, 50 connections | -0.1 % |
| PATCH, 1 connection | +0.6 % |
| merge, 50 connections | -1.0 % |
| big entity PATCH / create, 50 connections | +1.3 % / +1.0 % |

The reads, whose code is the same in A and B, moved by -1.8 to +2.7 % in the same runs: the writes are within the
noise. perfRun's entities have no instance of a datasetId, so no mark is written - this is the check every write
pays. A marked attribute writes 18 bytes more (`"@datasetIds": true`).

**Through the broker**, the harness of [corDB's declared indexes](#cordbs-declared-indexes) (the same
100 000 entities, wrk, 8 threads, 50 connections, off / on / on / off), `mongoc` release build `e3fef490`,
MongoDB 8.2.12 local, the database dropped before each run; the create rows a run of their own:

| scenario | off | on | on / off | µs a request, off -> on |
|---|---:|---:|---:|---:|
| `q=speed==1234` (10 matches), `limit=20` | 6 | 5 515 | x919 | 166 667 -> 181 |
| `q=speed>9990` (90 matches), `limit=20` | 24.5 | 1 827 | x75 | 40 816 -> 547 |
| `q=speed>9900` (990 matches, 1 %), `limit=20` | 81 | 224.5 | x2.8 | 12 346 -> 4 454 |
| `q=speed>5000` (50 %), `limit=20` | 163 | 161.5 | -0.9 % | |
| `q=speed>5000`, `count=true&limit=0` | under 1 | under 1 | | |
| `q=other==1234` (not declared), `limit=20` | 6 | 6 | | |
| no `q`, `limit=20` | 17 851 | 17 526 | -1.8 % | 56.0 -> 57.1 |
| `PATCH` of `other` | 20 391 | 20 261 | -0.6 % | 49.04 -> 49.36 (+0.31) |
| `PATCH` of `speed` | 20 447 | 19 436 | -4.9 % | 48.91 -> 51.45 (+2.54) |
| create, with `speed` | 58 874 | 56 426 | -4.2 % | 16.99 -> 17.72 (+0.74) |

The p50 of `q=speed==1234`: 3.85 s -> 8.6 ms.

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

### A store of one and ten million entities

perfRun's larger store at `PERF_BIG_ENTITIES=1000000` and `10000000`: perfRun's fixture entity (five
attributes, ~550 bytes of JSON), 1 % of type `R1`, 0.1 % of type `R01`, the rest `Common`. The store is
filled with batch creates of 500, one after the other from one client. Then, on random entities of the
whole store:

- `big_retrieve`: GET by id.
- `big_patch`: PATCH of one attribute.
- `big_create`: POST of new entities, measured last because it grows the store.

| | |
|---|---|
| Machine | AMD Ryzen 9 8940HX, 16 cores / 32 threads, 60 GiB; laptop, mains power, nothing else running |
| Broker | coraine 0.6.0-dev release build (main, b23e42f0), libmicrohttpd, not pinned to cores |
| Load | `test/perf/perfRun.sh <db>` with `PERF_BIG_ENTITIES`, its defaults otherwise (5 s x 3 per scenario, the median of the three, 50 connections) |
| mongoc | MongoDB 8.2 in a container (`mongo:8.2`) on the host network |
| corDB | `--dbDir` (`PERF_DB_DIR`) on the local NVMe, the durable configuration |
| Date | 2026-10-19 |

**Requests a second, c50 (p50 in µs):**

| Scenario | mongoc 1M | mongoc 10M | corDB 1M | corDB 10M |
|---|---:|---:|---:|---:|
| `big_retrieve` | 59 815 (756) | 59 167 (765) | 483 327 (84) | 496 014 (82) |
| `big_patch` | 18 880 (2 481) | 17 561 (2 670) | 91 261 (532) | 95 810 (442) |
| `big_create` | 48 215 (935) | 48 469 (924) | 84 597 (510) | 101 186 (451) |
| `big_r1_l20`, first page of a 1 % type | 16 670 (2 837) | 17 188 (2 750) | 78 832 (517) | 83 418 (522) |
| `big_r1_l20_o1000`, `offset=1000` | 1 409 (33 837) | 1 424 (33 418) | 2 093 (20 072) | 1 942 (19 584) |
| `big_r01_l20`, first page of a 0.1 % type | 16 474 (2 845) | 16 551 (2 834) | 22 462 (1 709) | 25 042 (1 556) |
| `big_r01_l20_o200`, `offset=200` | 5 300 (8 967) | 5 321 (8 922) | 814 (62 021) | 852 (41 815) |

The same requests on the same run's 100-entity fixture (`retrieve_c50`, `patch_c50`, `create_c50`):

| | mongoc 1M | mongoc 10M | corDB 1M | corDB 10M |
|---|---:|---:|---:|---:|
| `retrieve_c50` | 61 845 | 61 642 | 588 953 | 603 266 |
| `patch_c50` | 18 879 | 18 892 | 104 697 | 106 284 |
| `create_c50` | 50 505 | 49 525 | 87 405 | 88 313 |

**What holding the store costs**, read right after the fill:

| | mongoc 1M | mongoc 10M | corDB 1M | corDB 10M |
|---|---:|---:|---:|---:|
| Fill, seconds | 244.2 | 2 429.7 | 237.0 | 2 358.7 |
| Broker RSS | 125 MiB | 125 MiB | 2.04 GiB | 19.5 GiB |
| mongod RSS | 1.25 GiB | 1.77 GiB | — | — |
| MongoDB documents (`dataSize`) | 768 MiB | 7.5 GiB | — | — |
| MongoDB on disk (`storageSize` + `indexSize`, after `fsync`) | 128 MiB | 1.27 GiB | — | — |
| corDB `--dbDir` (`du`) | — | — | 602 MiB | 5.9 GiB |

- **Ten times the store, the same rates.** From 1M to 10M, every scenario moved by 6 % or less, with four
  exceptions. Dropped: mongoc's PATCH (-7 %) and corDB's R1 deep page (-7 %). Rose: corDB's create (+20 %)
  and its first R01 page (+11 %).
- **Against the 100-entity fixture, at 10M:** mongoc is 2-7 % slower. corDB's retrieve is 18 % slower,
  its PATCH 10 % slower, and its create 15 % faster.
- **mongoc: the broker does not grow with the store.** It holds 125 MiB at one million and at ten.
  mongod's RSS grows by half a GiB.
- **corDB: an entity is RAM, ~2.0 KiB of it** at one and ten million (2.14 and 2.05 KiB a resident
  entity). That is less than the [RAM](#ram) section's 3.5 KiB at 100 000. On disk an entity takes ~630
  bytes, in either size.
- **The fill time is the client's.** One `curl` per batch of 500, one after the other, takes the same
  ~240 µs an entity on both databases. It grows linearly with the store and does not measure the broker.
- **corDB's deep page of a rare type is slow.** `big_r01_l20_o200` runs at 814 / 852 a second, against
  mongoc's 5 300. The rate does not depend on the store's size, 1M or 10M, but on the offset into the
  type. Its first page, and the deep page of the 1 % type, are faster than mongoc's. With
  [corDB's type index](#cordbs-type-index) it runs at 103 560.

### corDB's type index

corDB keeps a list of each type's entities, in the store's order, and a query by one type (or one AND
group of types) walks that list instead of the store - corDB's README, "Queries by type - the type index".
Built with it (`COR_FEATURE_TYPE_INDEX=1`, the default) and without it, the same broker otherwise
(b23e42f0), on the machine and configuration of the section above, corDB with `--dbDir`, 2026-10-20.

**The larger store at one million entities**, one run each (without: the 1M run above):

| Scenario | without | with | |
|---|---:|---:|---:|
| `big_r01_l20_o200`, deep page of a 0.1 % type | 814 (62 021) | 103 560 (420) | x127 |
| `big_r1_l20_o1000`, deep page of a 1 % type | 2 093 (20 072) | 83 929 (525) | x40 |
| `big_r01_l20`, first page of a 0.1 % type | 22 462 (1 709) | 110 862 (398) | x4.9 |
| `big_r1_l20`, first page of a 1 % type | 78 832 (517) | 115 900 (361) | +47 % |
| `big_retrieve` | 483 327 | 488 477 | +1.1 % |
| `big_patch` | 91 261 | 90 128 | -1.2 % |
| `big_create` | 84 597 | 72 850 | -13.9 % |
| Broker RSS after the fill | 2.04 GiB | 2.09 GiB | +2.4 %, 51 bytes an entity |
| `--dbDir` | 602 MiB | 602 MiB | the index is not on disk |

**Every scenario, ABBA** (with, without, without, with), `PERF_BIG_ENTITIES=1000`, the mean of the two
runs of each build, requests a second at c50 unless named:

| Scenario | without | with | |
|---|---:|---:|---:|
| `rare_l20_o500`, deep page of a 1 % type in 100 100 entities | 4 679 | 96 866 | x21 |
| `rare_l20` / `rare_l100` | 81 158 / 18 662 | 115 104 / 26 779 | +42 % / +43 % |
| `query_c50` / `query_l100_c50` (a type that is the whole store) | 113 293 / 25 898 | 113 188 / 25 347 | -0.1 % / -2.1 % |
| `retrieve_c50` | 597 581 | 579 777 | -3.0 % |
| `patch_c50` / `patch_c1` | 107 049 / 46 733 | 102 148 / 45 583 | -4.6 % / -2.5 % |
| `merge_c50` | 71 509 | 69 797 | -2.4 % |
| `create_c50` / `create_c1` | 86 523 / 30 648 | 82 521 / 31 212 | -4.6 % / +1.8 % |
| `batch20create_c50` | 24 796 | 16 740 | -32.5 % |
| `batch20_c50` (batch update) | 28 122 | 26 254 | -6.6 % |
| `delete_c50` / `batch20delete_c50` | 161 930 / 53 091 | 152 548 / 50 298 | -5.8 % / -5.3 % |
| `patch_subs_c50` / `patch_10subs_c50` | 79 902 / 11 895 | 83 076 / 11 836 | +4.0 % / -0.5 % |

- **A deep page of a rare type costs about what its first page costs**: `rare_l20_o500` 96 866 against
  `rare_l20` 115 104, `big_r01_l20_o200` 103 560 against `big_r01_l20` 110 862.
- **A created entity costs ~0.2 µs more under the store's write lock**: its place at the end of its
  type's list, and in the map from an entity to that place. Where creates are bound by that lock it
  shows: a batch create of 20 at c50 without `--dbDir`, ABBA, 10 s a run, 62 707 -> 51 350 batches a
  second (-18 %), 0.80 -> 0.97 µs a created entity.
- **A batch create of 20 at c50 with `--dbDir` ran at one of two rates** in those 10 s runs: ~18 800 or
  ~14 000 batches a second. Without the index, nine runs gave 18 351 to 18 933. With it, 13 215 to
  17 917. With a busy loop of 150 cycles for each entity in the index's place (no memory touched),
  17 456 and 13 352: the write lock held a little longer put it on the lower rate. The cause was the
  snapshots, and corDB lets the writers in between a snapshot's slices now - next section. With that,
  the index costs a batch create what its time under the lock is: -9 %.
- **The other writes: -6.6 % to +1.8 %.** The other reads: -3.0 % to +1.1 %.

### Writers during a snapshot

A snapshot encodes the store in slices of 1 000 entities, each under the store's READ lock (about a
millisecond), and writers take the write lock between two slices. glibc's rwlock prefers readers: with
a fixed pause of 50 µs between two slices, the writers had the lock ~5 % of the time a snapshot lasted.
A store that grows is snapshotted most of the time - one is due each time the log since the last one is
as big as that snapshot - and a batch create in a growing store was bound by those pauses: two or three
batches of 20 in each.

corDB counts the writers that wait for the write lock, and between two slices a snapshot waits until
none waits, for at most three times as long as its slice took: writers have three quarters of the lock
or more while a snapshot lasts. With no writer waiting, the snapshot goes on at once.

Batch create of 20, c50, `--dbDir`, 10 s runs from an empty store (~4 M entities created in each run,
several snapshots), `wrk` with `test/perf/batchCreate.lua`, a write lock held ~20 µs a batch without the
type index and ~23 µs with it (measured with a probe around the lock), 2026-10-20:

| Batch create, batches a second | without the type index | with it | with vs without |
|---|---:|---:|---:|
| A pause of 50 µs between two slices (before) | 17 220 - 19 459 | 13 521 - 16 153 | -14 % to -28 %, at one of two rates |
| The writers waiting let in, for up to the slice's time | 22 303 / 22 455 | 19 495 / 19 644 | -12.6 % |
| The writers waiting let in, for up to three times the slice's time | 29 267 / 31 110 | 27 313 / 27 673 | -9 % |
| No snapshot in the run (`--dbSnapshotEvery 65536`), before | 37 057 / 37 557 | 33 393 / 34 521 | -8.5 % |
| `--dbLockPrefer writes`, before | 36 766 / 36 876 | 32 875 / 34 752 | -8.2 % |

- **Before, with the 50 µs pause, the write lock was held ~38 % of a run**, the broker used ~5 of 32
  hardware threads, and a writer waited 1.5 - 2.1 ms for the lock on average: the writers were bound by
  the pauses, not by the lock nor the CPU. Without a snapshot in the run the lock was held ~77 %.
- **Three times the slice's time, not once**: once gave the writers half the lock and 22 000 batches a
  second; three times, three quarters and 30 000. A snapshot under a steady stream of writers takes
  up to four times as long as with none.

**Every scenario, ABBA** (before, after, after, before: corDB 7592926 and dd826a1), the type index in both, `PERF_BIG_ENTITIES=1000`,
the mean of the two runs of each build, requests a second at c50 unless named:

| Scenario | before | after | | p99 µs before / after |
|---|---:|---:|---:|---:|
| `batch20create_c50` | 15 190 | 30 040 | +97.8 % | 16 936 / 6 258 |
| `create_c50` | 84 498 | 81 206 | -3.9 % | 19 396 / 3 071 |
| `big_create_c50` | 77 750 | 79 451 | +2.2 % | 24 671 / 3 361 |
| `retrieve_c50` | 597 706 | 611 715 | +2.3 % | |
| `patch_c50` / `patch_c1` | 104 932 / 46 888 | 103 779 / 46 707 | -1.1 % / -0.4 % | |
| `batch20delete_c50` | 51 361 | 50 011 | -2.6 % | |
| `patch_10subs_c50` | 11 560 | 11 927 | +3.2 % | |

- **The p99 of a create is a sixth to a seventh of what it was** (`create_c50`, `big_create_c50`), of a
  batch create under half.
- **The other scenarios: -1.8 % to +1.0 %.**

### corDB's declared indexes

A declared attribute (`--dbValueIndexes`, `/admin/indexes` - [Declared indexes](installation.md#declared-indexes))
is a value index in corDB's store, and a `q` term on it takes its candidates from the index instead of
walking the store - corDB's README, "Queries by value - declared indexes".

**The setup.** 2026-10-22, the release build of coraine (`main` after #444, and this section's changes) with
corDB `83b3323` (corDB#36: only the entities with a declared value in the index), corDB on disk (`--dbDir`, a
new directory each run), `--troe none`, the machine of [Profile-guided](#profile-guided-make-pgo). 100 000
entities of type `T`: `speed` = i mod 10 000 (ten entities a value), `other` = i mod 7 919; 20 000 more without
`speed` (`urn:F<i>`, for the `PUT`s); one query on `speed` after them (it builds the index). The same binary
with `--dbValueIndexes speed` (on) and without (off). wrk, 8 threads, 50 connections, 5 s x 3 a scenario (the
median); the runs in the order off, on, on, off, and the figure is the mean of the two runs of each. **µs a
request** is 1 / the rate: what one request takes of the broker at full load - a cost a request more or less
adds or saves.

| scenario | off | on | on / off | µs a request, off -> on |
|---|---:|---:|---:|---:|
| `q=speed==1234` (10 matches), `limit=20` | 900 | 374 775 | x416 | 1 111 -> 2.7 |
| `q=speed>9990` (90 matches), `limit=20` | 3 200 | 236 234 | x74 | 312.5 -> 4.2 |
| `q=speed>9900` (990 matches, 1 %), `limit=20` | 9 467 | 166 934 | x17.6 | 105.6 -> 6.0 |
| `q=speed>5000` (49 990 matches, 50 %), `limit=20` | 17 918 | 17 662 | -1.4 % | 55.8 -> 56.6 |
| `q=speed>5000`, `count=true&limit=0` | 702 | 1 136 | +62 % | 1 423 -> 880 |
| `q=other==1234` (not declared), `limit=20` | 651 | 645 | -0.9 % | 1 536 -> 1 550 |
| no `q`, `limit=20` | 268 577 | 262 305 | -2.3 % | 3.72 -> 3.81 |
| `PATCH` of `other` | 101 004 | 99 653 | -1.3 % | 9.90 -> 10.03 (+0.13) |
| `PATCH` of `speed` | 96 419 | 75 753 | -21.4 % | 10.37 -> 13.20 (+2.83) |
| create, with `speed` | 258 598 | 135 146 | -47.7 % | 3.87 -> 7.40 (+3.53) |
| `PUT` of an entity without `speed` | 140 888 | 145 326 | +3.1 % | 7.10 -> 6.88 |
| create, without `speed` (after the creates with it) | 324 030 | 292 666 | -9.7 % | 3.09 -> 3.42 (+0.33) |
| create, without `speed` (alone, a new broker each run) | 287 252 | 278 716 | -3.0 % | 3.48 -> 3.59 (+0.11) |

Each run, off / on / on / off: `q=speed==1234` 917 / 386 142 / 363 408 / 883; `PATCH` of `other` 101 127 /
99 933 / 99 373 / 100 881; `PATCH` of `speed` 96 067 / 75 412 / 76 094 / 96 771; create with `speed` 251 042 /
132 000 / 138 293 / 266 154; `PUT` without 133 094 / 145 713 / 144 938 / 148 682; create without, alone 292 422
/ 278 069 / 279 363 / 282 082. The p50 at 50 connections: `q=speed==1234` 38 ms -> 98 µs, `PATCH` of `speed`
470 -> 590 µs, create with `speed` 138 -> 333 µs.

- **A `q` on the attribute**: the fewer entities it matches, the larger the gain - x416 for 10 matches,
  x17.6 for 1 % of the store.
- **A page of a `q` that half the store matches** comes from the walk, not from the index: the walk stops at
  the page, the index gathers and sorts every candidate. corDB takes the index for a page when its
  candidates are at most `sqrt(page x entities)`, 64 at least. Without that limit (the index for every `q` it
  serves), corDB in RAM: 17 000 -> 5 500 a second.
- **A write of the declared attribute: +2.8 µs** (`PATCH`) - its entries out of and into the index, under the
  store's write lock. A create with it +3.5 µs: the index's entries and the entity's place in it.
- **An entity without a declared value is not in the index.** A write of another attribute: the names the
  write changed compared with the declared ones, +0.13 µs. A create or `PUT` of an entity without the
  attribute: a check of its declared attributes. In instructions (callgrind, 2 000 requests into 20 000
  entities): a `PATCH` of `other` 60 498 -> 60 906 (+408, +0.7 %), a create without `speed` 55 621 -> 56 267
  (+646, +1.2 %).
- **The creates without `speed` right after the creates with it: -9.7 %**; alone, -3.0 %, and the two runs
  without the index alone differ by 3.6 %. The creates with `speed` before them leave about 2 million entries
  in the index.
- **Queries that the index does not serve: within the noise** (-2.3 % to -0.9 %).
- The creates grow the store by about 1.2 million entities a run of 3 x 5 s, the index with it.

**corDB `9eb1354`**, where every entity was in the index, the same setup without the 20 000: `PATCH` of `other`
102 288 -> 100 584 (-1.7 %, +0.17 µs), of `speed` 97 887 -> 76 610 (-21.7 %), create with `speed` 255 664 ->
149 855 (-41.4 %); `q=speed==1234` 1 054 -> 374 813 (x356). With the 20 000, alone: create without `speed`
275 937 -> 247 034 (-10.5 %, +0.42 µs), `PUT` without it 140 940 -> 134 064 (-4.9 %, +0.36 µs).

**corDB `0096e48`**, where every write compared the entity's declared values with its entries, the same
setup: `PATCH` of `other` 101 896 -> 94 732 (-7.0 %, +0.74 µs), of `speed` 99 282 -> 76 274 (-23.2 %), create
251 645 -> 142 892 (-43.2 %); `q=speed==1234` 950 -> 344 265 (x362), 1 % 9 399 -> 165 266 (x17.6).

**corDB in RAM**, corDB `0096e48` without `--dbDir`: `q=speed==1234`
1 069 -> 375 893, 1 % 9 607 -> 168 878, 50 % with `limit=20` 18 240 -> 17 692, its count 813 -> 1 219, `q`
on `other` 757 -> 729, no `q` 270 679 -> 265 843; `PATCH` of `other` 133 752 -> 122 809 (-8.2 %), of `speed`
126 593 -> 90 113 (-28.8 %, +3.2 µs), create 389 127 -> 179 871 (-53.8 %, +3.0 µs). Creates alone
(`create` only, a new store each run): 390 000 off; on, 165 000 to 200 000 with `speed` in the entity, 330 000
to 385 000 without it.

### Building a declared index

What a change of the declarations costs on a running broker ([Declared indexes](installation.md#declared-indexes)).
2026-10-22, the release build of coraine `bbfb0db0` with corDB `2e8a455`, one tenant, 1 000 000 entities of type
`V`, each with `speed` (i mod 1 000) and `temp` (i mod 50), created in batches of 1 000; `--dbValueIndexes speed`,
then `POST /admin/indexes {"attribute": "temp"}` and its `DELETE`. Times are the request's (curl
`time_total`), on an idle broker.

| step | corDB (`--dbDir`, 3 runs) | mongoc (MongoDB 8.2 local, 3 runs) |
|---|---|---|
| the first `q=speed==5`: builds the store's index | 0.512 / 0.530 / 0.494 s | - (built at the tenant's setup) |
| the same `q`, built | 0.4 - 0.6 ms | - |
| `POST` `temp`: corDB builds both declarations' entries, mongoc creates `temp`'s two indexes | 0.862 / 0.869 / 0.887 s | 2.057 / 1.138 / 1.144 s |
| `DELETE` `temp`: corDB drops the store's index, mongoc `temp`'s two | 0.111 / 0.107 / 0.111 s | 0.0011 / 0.0009 / 0.0008 s |
| the next `q=speed==5`: corDB builds `speed`'s entries again | 0.483 / 0.432 / 0.511 s | - |

- **corDB keeps one index a store for all the declarations** (the entities, their order, an entry per
  value and declaration): a change of the declarations drops it, and builds it again - at once for a `POST`,
  by the next query that can use it after a `DELETE`. Its counters (`accesses`, `updates`) start from 0.
  The build reads the store under its read lock: the store's writes wait.
- **About 0.5 µs an entity and declaration** (one: 0.5 s, two: 0.87 s for 1 000 000), in every tenant's store
  a declaration covers.
- **mongoc**: a change creates or drops that declaration's MongoDB indexes only (the first `POST` of the
  three, 2.1 s, on a collection just loaded).

### Declared indexes of the TimescaleDB history

A declared attribute of the history (`--troeValueIndexes`, `/admin/troeIndexes` -
[TimescaleDB (the history)](installation.md#timescaledb-the-history)) is two partial indexes of `troe_attrs`. A
temporal query with a `q` term on it selects its entities from the index ("candidates"):
`FROM troe_entities WHERE entity_id IN (SELECT entity_id FROM troe_attrs WHERE <the term> AND <the window>)`
inside the selector's `DISTINCT ON (entity_id)`. Without such a term, it selects as before: the latest row of
every entity, each checked by `q` (`EXISTS`).

**The queries.** 2026-10-22, PostgreSQL 16.14 with TimescaleDB 2.29.2 (the `timescale/timescaledb-ha:pg16`
image), a history of 100 000 entities, 2 900 010 rows of `troe_attrs`. The entity selector's SQL as the broker
sends it, its execution time (`EXPLAIN ANALYZE`), two runs each, ms. The index: `(v_number) WHERE attr_name =
'<speed>'`. Both forms give the same entities.

| entities that match `q` | today, no index | today's SQL, the index | candidates, no index | candidates, the index |
|---|---:|---:|---:|---:|
| none, page of 20 / `count=true` | 618 / 618 | | | 0.1 / 0.1 |
| 10, page of 20 | 626 | 42 | | 1.2 |
| 10, `count=true` | 617 | 42 | | 1.1 |
| 1 000 (1 %), page of 20 | 15 | | 240 | 17 |
| 1 000 (1 %), `count=true` | 618 | 618 | | 45 |
| half, `count=true` | 490 | | | 357 |

- **The index needs the candidates form.** With the index and today's SQL, 10 matches take 42 ms, and 1 000
  take as long as without the index.
- **The candidates form needs the index.** Without it, the 1 % page takes 240 ms instead of 15 ms: the broker
  uses the candidates form only for a term an index serves (`pg_indexes`, read by the broker).
- **A page that matches early gains nothing**: today's selector walks the entities in `entity_id` order and
  stops at the page's end, so the 1 % page finds its 20 at once.
- The times are the SQL's. A request also reads the history of every entity on its page.

**The requests.** 2026-10-22, the same history; the release build of coraine (main after #448, built by
`coraine-build --source ~/git --release`), corDB in memory for the current state, `--troe timescale`. A temporal
query `GET /ngsi-ld/v1/temporal/entities?type=V&timerel=after&timeAt=2000-01-01T00:00:00Z&timeproperty=modifiedAt`
with the `q` and the page below; wrk, 4 threads, 8 connections, 15 s a query after 3 requests to warm. The runs
in the order off, on, on, off (two ABBA sets: the 10-match queries in their own); on: `--troeValueIndexes` on
`speed`, its two indexes made at the broker's start; off: no index of `speed` in the database (dropped before
the run - the broker would find it). Requests/s of the two runs of each, the median time of one request.

| `q`, matches | off: requests/s | p50 | on: requests/s | p50 | on / off |
|---|---|---:|---|---:|---:|
| `speed==5000`, 10, page of 20 | 9.92 / 10.52 | 774 ms | 638.4 / 637.6 | 12.3 ms | x62 |
| `speed==5000`, 10, `count=true` | 5.13 / 4.93 | 1.52 s | 606.3 / 604.8 | 13.0 ms | x120 |
| `speed==99999`, none | 10.72 / 10.52 | 757 ms | 11 358 / 11 209 | 0.7 ms | x1 062 |
| `speed==3`, 1 000 (1 %), page of 20 | 251.8 / 253.2 | 31.3 ms | 252.4 / 249.7 | 31.3 ms | 1.00 |
| `speed==3`, 1 000 (1 %), `count=true` | 9.99 / 9.92 | 800 ms | 161.6 / 161.0 | 49.5 ms | x16.2 |
| `speed<500`, half, `count=true` | 12.92 / 12.92 | 609 ms | 13.45 / 12.92 | 589 ms | +2.1 % |

- **A page of 10 matches: x62, the SQL's x520.** With the index, the request reads the history of its 10
  entities (12 ms), the selector 1.2 ms of it.
- `count=true` without the index runs the selector twice (the page and the count): 1.5 s.
- The half-match count gains nothing at request level (the SQL's 490 -> 357 ms is within the request's
  other work and the runs' spread).

**The writes.** 2026-10-22, the release build of coraine (`feat/troe-db-indexes`), corDB on disk (`--dbDir`, a
new directory each run) for the current state, `--troe timescale` on the same PostgreSQL (a new database each
run). 10 000 entities of type `V` with `speed` and `temp`, created in batches of 1 000. wrk, 8 threads, 50
connections, 10 s a scenario: a `PATCH` of `speed`, a `PATCH` of `temp`, a batch create of 20 new entities. The
runs in the order off, on, on, off; on: the two indexes of a declaration of `speed`, created after the broker's
start. **History rows/s**: the rows of `troe_attrs` the scenario wrote, divided by the time until the writers
had written them all; the figure is the mean of the two runs of each.

| scenario | requests/s off / on | history rows/s off | on | on / off |
|---|---|---:|---:|---:|
| `PATCH` of `speed` | 97 174 / 95 497 | 95 396 | 93 910 | -1.6 % |
| `PATCH` of `temp` | 92 198 / 89 916 | 90 081 | 87 979 | -2.3 % |
| batch create of 20 | 5 324 / 5 086 | 181 029 | 173 291 | -4.3 % |

Each run, off / on / on / off, history rows/s: `PATCH` of `speed` 97 444 / 95 375 / 92 444 / 93 348; `PATCH` of
`temp` 91 451 / 88 699 / 87 258 / 88 710; batch create 184 049 / 179 536 / 167 046 / 178 009. The first run is
the fastest in all three scenarios, and the `PATCH` of `temp`, whose rows the indexes do not hold, loses more than
the `PATCH` of `speed`: a difference of this size is within what the runs vary.

### mongo-c-driver 2.5.7 against 2.2.2

The same release build of coraine (`4ac22e50`), the driver swapped under the mongoc plugin (`LD_LIBRARY_PATH`;
2.5.7 built from source with zstd and SASL, as the image builds it), MongoDB 8.2.12 in a container,
`perfRun.sh mongoc` with the broker on one physical core (`PERF_BROKER_CORES=1`), AMD Ryzen 9 8940HX;
2026-10-29 18:54-20:00, ABBA order (2.2.2, 2.5.7, 2.5.7, 2.2.2). Requests/s, the mean of the two runs of each:

| requests/s | 2.2.2 | 2.5.7 | |
|---|---:|---:|---:|
| `query_c50` | 6 548 | 6 462 | -1.3 % |
| `query_c200` | 6 622 | 6 529 | -1.4 % |
| `query_l1_c50` | 18 058 | 18 478 | +2.3 % |
| `query_l100_c50` | 1 406 | 1 398 | -0.5 % |
| `retrieve_c50` | 23 892 | 23 457 | -1.8 % |
| `patch_c50` | 12 360 | 11 833 | -4.3 % |
| `patch_c1` | 3 131 | 3 052 | -2.5 % |
| `batch20_c50` | 1 806 | 1 759 | -2.6 % |
| `create_c50` | 23 032 | 22 670 | -1.6 % |
| `create_c1` | 6 902 | 6 760 | -2.1 % |
| `batch20create_c50` | 6 296 | 6 242 | -0.9 % |
| `merge_c50` | 15 526 | 15 256 | -1.7 % |
| `delete_c50` | 18 074 | 17 634 | -2.4 % |
| `batch20delete_c50` | 3 706 | 3 956 | +6.8 % |
| `patch_subs_c50` | 7 722 | 7 620 | -1.3 % |
| `patch_manysubs_c50` | 7 266 | 7 304 | +0.5 % |
| `patch_10subs_c50` | 3 454 | 3 540 | +2.5 % |
| `rare_l20_c50` | 6 780 | 6 698 | -1.2 % |
| `rare_l100_c50` | 1 736 | 1 716 | -1.2 % |
| `rare_l20_o500_c50` | 4 518 | 4 478 | -0.9 % |
| `big_r1_l20_c50` | 6 870 | 6 644 | -3.3 % |
| `big_r1_l20_o1000_c50` | 2 008 | 1 990 | -0.9 % |
| `big_r01_l20_c50` | 6 562 | 6 622 | +0.9 % |
| `big_r01_l20_o200_c50` | 6 418 | 6 349 | -1.1 % |
| `big_retrieve_c50` | 22 401 | 21 833 | -2.5 % |
| `big_patch_c50` | 11 798 | 11 474 | -2.7 % |
| `big_create_c50` | 20 956 | 20 148 | -3.9 % |

The larger store (300 000 entities) fills in 72 s with both; broker RSS 129 / 130 MB, data, storage and indexes
the same. Each run, 2.2.2 / 2.5.7 / 2.5.7 / 2.2.2: `patch_c50` 12 409 / 11 671 / 11 995 / 12 311;
`big_create_c50` 20 882 / 20 169 / 20 128 / 21 029; `batch20delete_c50` 3 707 / 3 882 / 4 031 / 3 704.

Instructions per request in the broker (`perf stat -e instructions:u,instructions:k`, 5 s of `wrk -t8 -c50`,
100 entities, broker on one core; the same ABBA order, one broker start each, 2026-10-29 20:20):

| per request | 2.2.2 | 2.5.7 | |
|---|---|---|---:|
| query, `limit=20`, user | 841 257 / 841 181 | 841 983 / 842 192 | +870 (+0.1 %) |
| retrieve, user | 90 166 / 89 360 | 91 372 / 91 039 | +1 440 (+1.6 %) |
| `PATCH` of `speed`, user | 175 261 / 175 629 | 180 497 / 179 756 | +4 680 (+2.7 %) |
| create, user | 139 964 / 139 608 | 141 463 / 141 008 | +1 450 (+1.0 %) |
| kernel, all four | 75 983-137 940 | 79 339-139 644 | within the starts' spread |

- **2.5.7 runs ~1 400 more user instructions per database operation**: a retrieve (one find) and a create (one
  insert) +1 440 / +1 450, a `PATCH` - more than one operation - +4 680. The kernel's share is unchanged.
- That is +2.7 % of a `PATCH`'s instructions and 1-2 % of a retrieve's and a create's; `patch_c50` -4.3 %,
  `big_create_c50` -3.9 % in requests/s, both 2.5.7 runs below both 2.2.2 runs.
- `batch20delete_c50` +6.8 %, both 2.5.7 runs above both 2.2.2 runs.
- 2.5.7 is what the fixes of 2.3-2.5 are in (JSON and BSON parsing, SCRAM and SASL, OCSP, name validation), and
  2.5 is the driver MongoDB names for server 9.0.

### The stack's patches to mongo-c-driver 2.5.7

The image, the CI and the `.deb` packages that bundle the driver build it with four patches
(corLibs' `tools/mongoc/patches/2.5.7`, each saying what it changes and keeps): `0001` a command's retry state
made at its first retry, `0002` retryReads / retryWrites read from the URI once per client, `0003` a reply's
error fields found in one pass, `0004` an implicit session without the transaction state it cannot use.
(`0005` only marks the changed files.)

Instructions per request in the broker, user space (`perf stat -e instructions:u`, 5 s of `wrk -t8 -c50`, 100
entities, broker on one core, MongoDB 8.2.12, the same release build of coraine; the mean of 2-4 broker starts,
each patch on top of the ones before it, 2026-10-29 20:20-21:05):

| per request, user | 2.5.7 | + `0001` | + `0002` | + `0003` | + `0004` | |
|---|---:|---:|---:|---:|---:|---:|
| query, `limit=20` | 841 911 | 841 444 | 838 942 | 836 403 | 835 662 | -6 249 (-0.7 %) |
| retrieve | 91 312 | 90 558 | 88 506 | 85 326 | 84 552 | -6 760 (-7.4 %) |
| `PATCH` of `speed` | 180 126 | 178 182 | 172 468 | 164 799 | 162 970 | -17 156 (-9.5 %) |
| create | 141 034 | 141 371 | 139 315 | 137 983 | 137 068 | -3 966 (-2.8 %) |

All four patches below 2.2.2's own count (the section above): a retrieve 84 552 against 89 763, a `PATCH`
162 970 against 175 445.

Requests/s, `perfRun.sh mongoc`, broker on one physical core, AMD Ryzen 9 8940HX; 2026-10-29 21:12-22:18, ABBA
(2.5.7, patched, patched, 2.5.7), the mean of the two runs of each, and how far apart the two 2.5.7 runs were:

| requests/s | 2.5.7 | patched | | 2.5.7 runs |
|---|---:|---:|---:|---:|
| `query_c50` | 6 538 | 6 462 | -1.1 % | 1.9 % apart |
| `query_c200` | 6 562 | 6 678 | +1.8 % | 3.3 % apart |
| `query_l1_c50` | 18 758 | 18 834 | +0.4 % | 0.5 % apart |
| `query_l100_c50` | 1 410 | 1 400 | -0.7 % | 2.1 % apart |
| `retrieve_c50` | 23 770 | 23 247 | -2.2 % | 1.1 % apart |
| `patch_c50` | 12 174 | 12 025 | -1.2 % | 2.4 % apart |
| `patch_c1` | 3 088 | 3 125 | +1.2 % | 2.5 % apart |
| `batch20_c50` | 1 796 | 1 791 | -0.3 % | 2.4 % apart |
| `create_c50` | 23 111 | 22 454 | -2.8 % | 5.6 % apart |
| `create_c1` | 6 856 | 6 882 | +0.4 % | 4.0 % apart |
| `batch20create_c50` | 6 307 | 6 202 | -1.7 % | 2.3 % apart |
| `merge_c50` | 15 193 | 15 404 | +1.4 % | 3.9 % apart |
| `delete_c50` | 17 629 | 18 171 | +3.1 % | 4.0 % apart |
| `batch20delete_c50` | 4 115 | 3 686 | -10.4 % | 18.0 % apart |
| `patch_subs_c50` | 7 643 | 7 766 | +1.6 % | 0.1 % apart |
| `patch_manysubs_c50` | 7 078 | 7 294 | +3.1 % | 6.0 % apart |
| `patch_10subs_c50` | 3 542 | 3 574 | +0.9 % | 2.8 % apart |
| `rare_l20_c50` | 6 803 | 6 664 | -2.0 % | 4.5 % apart |
| `rare_l100_c50` | 1 736 | 1 707 | -1.6 % | 3.5 % apart |
| `rare_l20_o500_c50` | 4 164 | 4 166 | +0.0 % | 19.7 % apart |
| `big_r1_l20_c50` | 6 808 | 6 760 | -0.7 % | 1.1 % apart |
| `big_r1_l20_o1000_c50` | 1 954 | 2 002 | +2.4 % | 3.7 % apart |
| `big_r01_l20_c50` | 6 657 | 6 642 | -0.2 % | 0.6 % apart |
| `big_r01_l20_o200_c50` | 6 344 | 6 407 | +1.0 % | 1.6 % apart |
| `big_retrieve_c50` | 22 071 | 22 242 | +0.8 % | 2.3 % apart |
| `big_patch_c50` | 11 416 | 11 682 | +2.3 % | 0.1 % apart |
| `big_create_c50` | 20 498 | 20 676 | +0.9 % | 3.2 % apart |

- **No scenario is faster by more than its runs' spread.** The broker's instructions per request fall 7-10 % on a
  retrieve and a `PATCH`; a request's time is mostly MongoDB's and the kernel's, and requests/s do not show it.
- `retrieve_c50` -2.2 %: both patched runs (23 253 / 23 241) below both 2.5.7 runs (23 637 / 23 903).
- `batch20delete_c50` and `rare_l20_o500_c50`: the two 2.5.7 runs 18-20 % apart - no reading.
- The larger store (300 000 entities): filled in 72 s, broker RSS 130 MB, data, storage and indexes the same with
  both.
- The driver's own tests (`test-libmongoc`) of what the patches touch - retryable reads and writes, the retry
  backoff, sessions, transactions, client, cursor, collection, bulk, CRUD, command, cluster, RPC: 909 tests, the
  same result for each with and without the patches (672 pass, 184 skipped, 53 fail on both against a standalone
  MongoDB 8.2 - replica-set and TLS-log tests).

## The HTTP server

### libmicrohttpd 1.0.10 against 1.0.2

The same release build of coraine (`ef19b7e1`), libmicrohttpd swapped under it (`LD_LIBRARY_PATH`; both built by
[corLibs](https://github.com/SEAMWARE/corLibs)' `tools/mhd/mhdBuild.sh`, the broker mapping `libmicrohttpd.so.12.62.2` / `.12.62.8`), corDB in RAM,
`perfRun.sh` with the broker on one physical core (`PERF_BROKER_CORES=1`), AMD Ryzen 9 8940HX; 2026-10-13
01:50-02:20, 1.0.2 first.

| requests/s | 1.0.2 | 1.0.10 | p99 µs 1.0.2 / 1.0.10 |
|---|---:|---:|---:|
| `query_c50` | 11 254 | 11 447 (+1.7 %) | 4614 / 4258 |
| `query_c200` | 11 234 | 11 470 (+2.1 %) | 19238 / 17675 |
| `query_l1_c50` | 74 173 | 76 885 (+3.7 %) | 735 / 704 |
| `query_l100_c50` | 2 514 | 2 528 (+0.6 %) | 19287 / 19082 |
| `retrieve_c50` | 83 912 | 88 548 (+5.5 %) | 644 / 603 |
| `patch_c50` | 108 766 | 110 459 (+1.6 %) | 523 / 482 |
| `patch_c1` | 49 968 | 49 585 (-0.8 %) | 24 / 24 |
| `batch20_c50` | 11 204 | 10 961 (-2.2 %) | 4435 / 4546 |
| `create_c50` | 65 923 | 65 854 (-0.1 %) | 832 / 836 |
| `create_c1` | 35 308 | 32 132 (-9.0 %) | 34 / 100 |
| `batch20create_c50` | 8 089 | 7 796 (-3.6 %) | 6210 / 6899 |
| `merge_c50` | 81 833 | 77 961 (-4.7 %) | 610 / 710 |
| `delete_c50` | 124 293 | 125 860 (+1.3 %) | 433 / 445 |
| `batch20delete_c50` | 24 898 | 24 893 (-0.0 %) | 3523 / 3009 |
| `patch_subs_c50` | 22 020 | 21 942 (-0.4 %) | 2336 / 2384 |
| `patch_manysubs_c50` | 20 732 | 21 005 (+1.3 %) | 2526 / 2529 |
| `rare_l20_c50` | 8 103 | 8 000 (-1.3 %) | 6010 / 6052 |
| `rare_l100_c50` | 1 773 | 1 759 (-0.8 %) | 27210 / 27490 |
| `rare_l20_o500_c50` | 577 | 566 (-1.9 %) | 129394 / 89294 |
| `big_r1_l20_c50` | 8 150 | 8 091 (-0.7 %) | 5948 / 5968 |
| `big_r1_l20_o1000_c50` | 151 | 146 (-3.3 %) | 721278 / 757690 |
| `big_r01_l20_c50` | 2 438 | 2 423 (-0.6 %) | 19786 / 19903 |
| `big_r01_l20_o200_c50` | 52 | 51 (-1.9 %) | 1939536 / 1950114 |

The scenarios that moved by more than the run's spread, measured again (2026-10-13 02:45-03:20):

| | 1.0.2 | 1.0.10 |
|---|---|---|
| `merge_c50`, requests/s, three rounds, 1.0.2 first in each | 96 790 / 96 751 / 96 693 | 95 271 / 95 210 / 94 426 |
| `merge_c50`, requests/s, ABBA order (each version first and second) | 99 139 / 94 699 / 94 081 / 100 727 | 94 760 / 93 980 / 95 525 / 94 456 |
| `patch_c50`, the same ABBA runs | 109 449 / 106 147 / 107 303 / 110 470 | 108 205 / 106 461 / 107 670 / 107 714 |
| `create_c1`, requests/s (p99 µs) | 35 544 / 36 677 / 34 763 (91 / 39 / 61) | 35 821 / 33 594 / 35 718 (45 / 40 / 36) |
| **user instructions per request** (`perf stat`, two broker starts each), merge | 63 815 / 63 708 | 64 054 / 64 161 |
| **user instructions per request**, PATCH | 44 988 / 44 891 | 45 284 / 45 460 |
| kernel instructions per request, both | 18 667-18 745 | 18 660-18 795 |

- **1.0.10 runs ~400 more instructions per request** - the same for a merge (+346) and a PATCH (+432), as the
  HTTP server's own work is: about 1 % of a PATCH's instructions, 0.5 % of a merge's, ~0.1 µs.
- **The requests/s differences are the broker start's, not the library's**: one start of the same build and
  library differs from the next by up to 6 % (`merge_c50` 94 081 ... 100 727 on 1.0.2) - a measurement that runs
  one version first in every round reads that as a difference (`merge_c50` -1.8 % in the first three rounds).
  `create_c1` overlaps.
- **PATCH, retrieve, query, notifications**: within -1.4 ... +5.5 % in perfRun and the direct comparisons.
- 1.0.10 is what 1.0.4 / 1.0.5 (HTTP request smuggling, CWE-444) and 1.0.7-1.0.9 (security fixes) are in.

### The HTTP server's connection memory

libmicrohttpd keeps a memory pool per connection - the request's line and headers, its own header table, the
response's headers; a body passes through it in pieces - and **zeroes the whole pool at the end of every
request** (`MHD_pool_reset`), and the read buffer's unused tail on the switch from receiving to sending
(`MHD_pool_deallocate`). With its default 32 kB that `memset` was 4.3-4.7 % of every small request's cycles
(`perf record`, PATCH and retrieve at 50 connections, with and without `--auth`), nearly all of it on memory
never written. `--httpConnectionMemory` (kB, default 12) sets the pool - the zeroing stays.

The pool size alone, libmicrohttpd 1.0.10 built with each default (`MHD_POOL_SIZE_DEFAULT`), PATCH / retrieve at
50 connections, 10 s, broker on one core, the mean of three alternating rounds (2026-10-13 02:00):

| pool | PATCH | retrieve | headers up to | URL up to |
|---|---:|---:|---|---|
| 32 kB | 100 560 | 123 133 | more than 20 kB | more than 18 kB |
| 16 kB | 101 204 (+0.6 %) | 124 980 (+1.5 %) | 14 kB | 13 kB |
| **12 kB** | 102 891 (+2.3 %) | 127 133 (+3.2 %) | 11 kB | 9 kB |
| 8 kB | 103 583 (+3.0 %) | 127 192 (+3.3 %) | 7 kB | 6 kB |
| no zeroing at all (measurement only) | +2.6 ... +6.3 % | +4.9 ... +6.4 % | | |

The rounds spread by ±2 %: 12 kB and 8 kB are the same within it. A larger request than the limits is refused by
libmicrohttpd itself - 431 / 414 - and said in the broker's log ([Request size](installation.md#request-size)); a
1 000-entity response (477 kB) and 45 kB batch bodies pass at every size.

perfRun, the same build and libmicrohttpd 1.0.10, `--httpConnectionMemory 32` and `12` (2026-10-13 02:05-02:33,
12 kB last; the last column against libmicrohttpd 1.0.2 with 32 kB - today's):

| requests/s | 32 kB | 12 kB | 12 kB, 1.0.10 vs 32 kB, 1.0.2 |
|---|---:|---:|---:|
| `query_c50` | 11 447 | 11 496 (+0.4 %) | +2.2 % |
| `query_c200` | 11 470 | 11 411 (-0.5 %) | +1.6 % |
| `query_l1_c50` | 76 885 | 79 129 (+2.9 %) | +6.7 % |
| `query_l100_c50` | 2 528 | 2 528 (+0.0 %) | +0.6 % |
| `retrieve_c50` | 88 548 | 90 267 (+1.9 %) | +7.6 % |
| `patch_c50` | 110 459 | 111 395 (+0.8 %) | +2.4 % |
| `patch_c1` | 49 585 | 51 258 (+3.4 %) | +2.6 % |
| `batch20_c50` | 10 961 | 11 208 (+2.3 %) | +0.0 % |
| `create_c50` | 65 854 | 67 181 (+2.0 %) | +1.9 % |
| `create_c1` | 32 132 | 36 778 (+14.5 %) | +4.2 % |
| `batch20create_c50` | 7 796 | 7 915 (+1.5 %) | -2.2 % |
| `merge_c50` | 77 961 | 83 334 (+6.9 %) | +1.8 % |
| `delete_c50` | 125 860 | 129 448 (+2.9 %) | +4.1 % |
| `batch20delete_c50` | 24 893 | 25 106 (+0.9 %) | +0.8 % |
| `patch_subs_c50` | 21 942 | 22 598 (+3.0 %) | +2.6 % |
| `patch_manysubs_c50` | 21 005 | 20 822 (-0.9 %) | +0.4 % |
| `rare_l20_c50` | 8 000 | 8 092 (+1.1 %) | -0.1 % |
| `rare_l100_c50` | 1 759 | 1 756 (-0.2 %) | -1.0 % |
| `rare_l20_o500_c50` | 566 | 426 (-24.7 %) | -26.2 % |
| `big_r1_l20_c50` | 8 091 | 8 034 (-0.7 %) | -1.4 % |
| `big_r1_l20_o1000_c50` | 146 | 114 (-21.9 %) | -24.5 % |
| `big_r01_l20_c50` | 2 423 | 2 237 (-7.7 %) | -8.2 % |
| `big_r01_l20_o200_c50` | 51 | 43 (-15.7 %) | -17.3 % |

- **The small requests gain**: retrieve +1.9 % (+7.6 % with the version), `limit=1` query +2.9 % (+6.7 %),
  PATCH +0.8 % (+2.4 %), notifying PATCH +3.0 % (+2.6 %).
- **The deep pages** (`rare_l20_o500` -24.7 %, `big_r1_l20_o1000` -21.9 %, `big_r01_l20_o200` -15.7 %) are not the
  pool: alternating 32 and 12 kB on the same 100 000-entity store (2026-10-13 02:40, three rounds each),
  `type=Rare&limit=20&offset=500` runs at 439 / 440 / 473 and 442 / 462 / 452 requests/s, and `limit=20` at
  7 919 / 7 882 / 7 540 and 7 894 / 7 878 / 8 036. Each of those requests walks the store for 2-20 ms; they are
  perfRun's last scenarios, and the last configuration of a run, where they read low here and in
  [What authorization costs](#what-authorization-costs)' first run.

**Not done**: zeroing only what a request wrote. The pool's front is the read buffer and the write buffer, and
the write buffer's writers go past the offset they commit (`get_date_header()` writes `header[37] = 0`, and on
failure after a partial date; `MHD_uint16_to_str()` writes digits before it finds the buffer too small) - an exact
high-water mark would mean instrumenting libmicrohttpd's writers one by one. Offered upstream instead.

## What authorization costs

`--auth fdsc` ([Authorization](authorization.md)) against no `--auth`, every scenario of `perfRun.sh`, with
[`test/perf/authOverhead.sh`](https://github.com/SEAMWARE/coraine/blob/main/test/perf/authOverhead.sh).

### Security off and on, 1 to 20 policies - main, one sitting

Every configuration below in one sitting on one build, ten `perfRun.sh` runs in this order: none, `token`,
`p1`, `p10`, `p20`, none, `n2`, `n10`, `n20`, none. The three no-auth runs are the control for the 2.5 hours:
every percentage is against their median, and their spread is the third column.

| | |
|---|---|
| Build | Release (`-O2`), coraine main `c44d4b4e`; libmicrohttpd 1.0.10 with corLibs' header patch (`tools/mhd/patches/1.0.10/0001-header-value-fastpath.patch`, [The token's size](#the-tokens-size)) |
| Machine, placement | AMD Ryzen 9 8940HX; the broker pinned to one physical core (`PERF_BROKER_CORES=1`), wrk and the notification receiver on the other fifteen; nothing else running |
| Store | corDB in RAM (`--troe none`); perfRun's fixture - 100 entities of type `Vehicle`, five attributes, ~550 bytes each - and its larger stores for the `rare_*` and `big_*` scenarios |
| Token | one RS256 access token as VCVerifier issues it - a `UserCredential`, 696 bytes (`test/funcTests/tools/ftJwt.py`) - on every request of every configuration but none |
| Runs | 2026-10-13 18:06-20:36, 15 minutes per configuration; perfRun's 5 s x 3 per scenario, the median. Every answer of every run was a 2xx: `perfRun.sh` stops a run on the first that is not |

**The configurations.** "0 policies" is security off: with `--auth fdsc` and no policy, nothing is allowed
([How a decision is made](authorization.md#42-how-a-decision-is-made)) - there is no run "on, with 0 policies"
to make.

| | `--auth` | decided | the policies, in their order |
|---|---|---|---|
| none | - | - | - (no `Authorization` header either) |
| `token` | - | - | - : the token sent, never read - what receiving it costs |
| `p1` | `fdsc` | per request, as the Data Space Connector decides | `allowAll` |
| `p10`, `p20` | `fdsc` | per request | `other1` ... `other9` (`other19`), then `allowAll` |
| `n2` | `fdsc --fdscNgsild` | on each entity a request reads or writes, and each notification for its subscription's owner ([§ 6](authorization.md#6---fdscngsild---decided-on-the-entity)) | `endpoints`, `storeTypes` |
| `n10`, `n20` | `fdsc --fdscNgsild` | on each entity | `other1` ... `other8` (`other18`), then `endpoints`, `storeTypes` |

Every policy is an `odrl:use` permission (every method: the method mask skips none of them) for
`vc:any` (any holder of a token that verifies), on an `odrl:AssetCollection` - what it allows is its one
refinement:

- **`other<K>`** - `ngsi-ld:entityType` `odrl:eq` `"Other<K>"`, a type nothing in the store has. It never
  holds for a measured request: a set of N puts the allowing one last, so each request is checked against
  all N before one lets it in - the most N policies cost a request that is allowed. It is the per-type
  policy a data space deploys, one per entity type a consumer may use.
  ```json
  {"@context": {"odrl": "http://www.w3.org/ns/odrl/2/"}, "@type": "odrl:Policy", "odrl:uid": "urn:perf:policy:other1",
   "odrl:permission": {"odrl:assignee": {"@id": "vc:any"}, "odrl:action": {"@id": "odrl:use"},
     "odrl:target": {"@type": "odrl:AssetCollection",
       "odrl:refinement": [{"@type": "odrl:Constraint", "odrl:leftOperand": "ngsi-ld:entityType",
                            "odrl:operator": {"@id": "odrl:eq"}, "odrl:rightOperand": "Other1"}]}}}
  ```
- **`allowAll`** - `http:path` `odrl:regex` `"^/"`: every path, so every request.
  ```json
  "odrl:target": {"@type": "odrl:AssetCollection", "odrl:source": "urn:asset",
    "odrl:refinement": [{"@type": "odrl:Constraint", "odrl:leftOperand": "http:path",
                         "odrl:operator": {"@id": "odrl:regex"}, "odrl:rightOperand": "^/"}]}
  ```
- **`endpoints`** - `http:path` `odrl:regex` `"^/ngsi-ld/v1/(subscriptions|types)"`: the operations
  `--fdscNgsild` does not decide entity by entity - perfRun's subscriptions and its readiness probe
  (`GET /types`).
  ```json
  "odrl:refinement": [{"@type": "odrl:Constraint", "odrl:leftOperand": "http:path",
                       "odrl:operator": {"@id": "odrl:regex"}, "odrl:rightOperand": "^/ngsi-ld/v1/(subscriptions|types)"}]
  ```
- **`storeTypes`** - `ngsi-ld:entityType` `odrl:isAnyOf` `["Vehicle", "Common", "Rare", "R1", "R01"]`, the
  types of perfRun's stores: every entity, decided on its stored type (a create: the body's), expanded.
  ```json
  "odrl:refinement": [{"@type": "odrl:Constraint", "odrl:leftOperand": "ngsi-ld:entityType",
                       "odrl:operator": {"@id": "odrl:isAnyOf"}, "odrl:rightOperand": ["Vehicle", "Common", "Rare", "R1", "R01"]}]
  ```

There is no `n1`: without `endpoints` perfRun's subscriptions are refused, without `storeTypes` every entity.
`test/perf/authOverhead.sh` writes each set to `<outDir>/policies-<N>.json`.

**The scenarios** (`test/perf/perfRun.sh`; paths under `/ngsi-ld/v1`; `_c50` = 50 connections):

| scenario | each request |
|---|---|
| `query_c50`, `query_c200` | `GET /entities?type=Vehicle&limit=20` - a 20-entity page; 50 / 200 connections |
| `query_l1_c50`, `query_l100_c50` | the same, `limit=1` / `limit=100` |
| `retrieve_c50` | `GET /entities/urn:ngsi-ld:Vehicle:7` |
| `patch_c50`, `patch_c1` | `PATCH /entities/{id}/attrs`, one attribute; 50 / 1 connection |
| `batch20_c50` | `POST /entityOperations/update`, 20 entities |
| `create_c50`, `create_c1` | `POST /entities` |
| `batch20create_c50` | `POST /entityOperations/create`, 20 entities |
| `merge_c50` | `PATCH /entities/{id}` |
| `delete_c50`, `batch20delete_c50` | `DELETE /entities/{id}` / `POST /entityOperations/delete` of 20, from a pool of 40 000 entities |
| `patch_subs_c50` | the PATCH, one subscription per entity: each PATCH notifies one subscriber |
| `patch_manysubs_c50` | the same past 110 more subscriptions that do not match (100 to other types, 10 to `Vehicle` whose `q` does not hold) |
| `patch_10subs_c50` | 9 more to `Vehicle`: ten notifications per PATCH (10.0-10.2 received in every run) |
| `rare_l20_c50`, `rare_l100_c50`, `rare_l20_o500_c50` | 100 000 entities, 1 % of type `Rare`: its first page of 20, of 100, and `offset=500` |
| `big_r1_*`, `big_r01_*` | 300 000 entities, 1 % `R1` and 0.1 % `R01`: a first page and a deep one (`offset=1000` / `offset=200`) |


**requests/s** - the median of the three no-auth runs, its spread, and each configuration against it:

| requests/s | none (median of 3) | the 3 none runs | `token` | `p1` | `p10` | `p20` | `n2` | `n10` | `n20` |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `query_c50` | 11 447 | -0.7 .. +1.7 % | 11 401 (-0.4 %) | 11 451 (+0.0 %) | 11 296 (-1.3 %) | 11 327 (-1.0 %) | 10 399 (-9.2 %) | 10 322 (-9.8 %) | 10 219 (-10.7 %) |
| `query_c200` | 11 346 | -1.5 .. +1.0 % | 11 336 (-0.1 %) | 11 244 (-0.9 %) | 11 190 (-1.4 %) | 11 229 (-1.0 %) | 10 367 (-8.6 %) | 10 347 (-8.8 %) | 10 150 (-10.5 %) |
| `query_l1_c50` | 79 029 | -2.9 .. +1.9 % | 77 726 (-1.6 %) | 75 136 (-4.9 %) | 74 059 (-6.3 %) | 74 800 (-5.4 %) | 71 065 (-10.1 %) | 71 141 (-10.0 %) | 70 611 (-10.7 %) |
| `query_l100_c50` | 2 476 | -16.4 .. +0.3 % | 2 511 (+1.4 %) | 2 492 (+0.6 %) | 2 483 (+0.3 %) | 2 480 (+0.2 %) | 2 284 (-7.8 %) | 2 273 (-8.2 %) | 2 235 (-9.7 %) |
| `retrieve_c50` | 89 975 | -3.8 .. +1.4 % | 86 909 (-3.4 %) | 85 018 (-5.5 %) | 83 954 (-6.7 %) | 84 864 (-5.7 %) | 80 203 (-10.9 %) | 80 372 (-10.7 %) | 81 253 (-9.7 %) |
| `patch_c50` | 110 348 | -0.3 .. +2.4 % | 108 029 (-2.1 %) | 103 062 (-6.6 %) | 100 993 (-8.5 %) | 100 970 (-8.5 %) | 95 376 (-13.6 %) | 96 704 (-12.4 %) | 95 327 (-13.6 %) |
| `patch_c1` | 49 268 | -1.8 .. +1.1 % | 49 289 (+0.0 %) | 47 025 (-4.6 %) | 42 663 (-13.4 %) | 46 712 (-5.2 %) | 45 940 (-6.8 %) | 43 825 (-11.0 %) | 46 799 (-5.0 %) |
| `batch20_c50` | 11 183 | -8.7 .. +0.2 % | 11 171 (-0.1 %) | 10 915 (-2.4 %) | 10 824 (-3.2 %) | 10 767 (-3.7 %) | 9 955 (-11.0 %) | 9 686 (-13.4 %) | 9 625 (-13.9 %) |
| `create_c50` | 66 576 | -0.1 .. +0.1 % | 64 317 (-3.4 %) | 63 047 (-5.3 %) | 60 138 (-9.7 %) | 60 310 (-9.4 %) | 60 890 (-8.5 %) | 59 892 (-10.0 %) | 59 526 (-10.6 %) |
| `create_c1` | 36 350 | -0.4 .. +2.0 % | 35 576 (-2.1 %) | 34 637 (-4.7 %) | 33 646 (-7.4 %) | 33 390 (-8.1 %) | 32 962 (-9.3 %) | 33 240 (-8.6 %) | 33 590 (-7.6 %) |
| `batch20create_c50` | 7 807 | -0.3 .. +0.7 % | 7 623 (-2.4 %) | 7 713 (-1.2 %) | 7 622 (-2.4 %) | 7 545 (-3.4 %) | 7 235 (-7.3 %) | 7 145 (-8.5 %) | 7 109 (-8.9 %) |
| `merge_c50` | 79 120 | -1.2 .. +3.1 % | 77 533 (-2.0 %) | 76 722 (-3.0 %) | 76 827 (-2.9 %) | 75 312 (-4.8 %) | 73 515 (-7.1 %) | 71 459 (-9.7 %) | 71 848 (-9.2 %) |
| `delete_c50` | 129 156 | -0.2 .. +1.3 % | 125 729 (-2.7 %) | 121 446 (-6.0 %) | 116 133 (-10.1 %) | 113 273 (-12.3 %) | 113 234 (-12.3 %) | 111 476 (-13.7 %) | 105 177 (-18.6 %) |
| `batch20delete_c50` | 25 300 | -2.3 .. +0.5 % | 24 811 (-1.9 %) | 24 481 (-3.2 %) | 24 688 (-2.4 %) | 24 604 (-2.8 %) | 21 532 (-14.9 %) | 20 958 (-17.2 %) | 20 449 (-19.2 %) |
| `patch_subs_c50` | 22 323 | -0.7 .. +1.3 % | 21 977 (-1.5 %) | 21 907 (-1.9 %) | 21 604 (-3.2 %) | 21 519 (-3.6 %) | 21 000 (-5.9 %) | 21 067 (-5.6 %) | 21 014 (-5.9 %) |
| `patch_manysubs_c50` | 20 858 | -0.2 .. +2.0 % | 20 794 (-0.3 %) | 20 520 (-1.6 %) | 20 447 (-2.0 %) | 20 018 (-4.0 %) | 20 136 (-3.5 %) | 19 941 (-4.4 %) | 19 924 (-4.5 %) |
| `patch_10subs_c50` | 2 995 | -0.1 .. +1.2 % | 2 919 (-2.5 %) | 2 967 (-0.9 %) | 2 993 (-0.1 %) | 2 979 (-0.5 %) | 2 938 (-1.9 %) | 2 910 (-2.8 %) | 2 912 (-2.8 %) |
| `rare_l20_c50` | 8 045 | -1.3 .. +0.6 % | 7 889 (-1.9 %) | 7 996 (-0.6 %) | 7 954 (-1.1 %) | 7 905 (-1.7 %) | 7 545 (-6.2 %) | 7 443 (-7.5 %) | 7 369 (-8.4 %) |
| `rare_l100_c50` | 1 764 | -0.2 .. +0.5 % | 1 726 (-2.2 %) | 1 773 (+0.5 %) | 1 766 (+0.1 %) | 1 759 (-0.3 %) | 1 661 (-5.8 %) | 1 647 (-6.6 %) | 1 627 (-7.8 %) |
| `rare_l20_o500_c50` | 561 | -0.9 .. +1.4 % | 549 (-2.1 %) | 551 (-1.8 %) | 563 (+0.4 %) | 554 (-1.2 %) | 567 (+1.1 %) | 555 (-1.1 %) | 533 (-5.0 %) |
| `big_r1_l20_c50` | 8 097 | -0.9 .. +0.5 % | 8 052 (-0.6 %) | 7 916 (-2.2 %) | 8 029 (-0.8 %) | 7 944 (-1.9 %) | 7 579 (-6.4 %) | 7 522 (-7.1 %) | 7 419 (-8.4 %) |
| `big_r1_l20_o1000_c50` | 146 | -4.8 .. +1.4 % | 145 (-0.7 %) | 148 (+1.4 %) | 147 (+0.7 %) | 146 (+0.0 %) | 146 (+0.0 %) | 145 (-0.7 %) | 144 (-1.4 %) |
| `big_r01_l20_c50` | 2 427 | -6.6 .. +0.0 % | 2 413 (-0.6 %) | 2 413 (-0.6 %) | 2 387 (-1.6 %) | 2 435 (+0.3 %) | 2 368 (-2.4 %) | 2 357 (-2.9 %) | 2 371 (-2.3 %) |
| `big_r01_l20_o200_c50` | 49 | +0.0 .. +4.1 % | 50 (+2.0 %) | 50 (+2.0 %) | 50 (+2.0 %) | 49 (+0.0 %) | 49 (+0.0 %) | 50 (+2.0 %) | 50 (+2.0 %) |

The broker's time per request on its one core (1 / requests/s), and what each configuration adds to it:

| µs per request, added to none's | none | `token` | `p1` | `p10` | `p20` | `n2` | `n10` | `n20` |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `patch_c50` | 9.06 | +0.19 | +0.64 | +0.84 | +0.84 | +1.42 | +1.28 | +1.43 |
| `merge_c50` | 12.64 | +0.26 | +0.40 | +0.38 | +0.64 | +0.96 | +1.36 | +1.28 |
| `retrieve_c50` | 11.11 | +0.39 | +0.65 | +0.80 | +0.67 | +1.35 | +1.33 | +1.19 |
| `query_l1_c50` - 1 entity | 12.65 | +0.21 | +0.66 | +0.85 | +0.72 | +1.42 | +1.40 | +1.51 |
| `create_c50` | 15.02 | +0.53 | +0.84 | +1.61 | +1.56 | +1.40 | +1.68 | +1.78 |
| `query_c50` - 20 entities | 87.36 | +0.35 | -0.03 | +1.17 | +0.93 | +8.80 | +9.52 | +10.50 |
| `batch20_c50` - 20 entries | 89.42 | +0.10 | +2.20 | +2.97 | +3.45 | +11.03 | +13.82 | +14.47 |
| `query_l100_c50` - 100 entities | 403.88 | -5.63 | -2.59 | -1.14 | -0.65 | +33.95 | +36.07 | +43.55 |

- **Receiving the token**: +0.2-0.5 µs per request (`token`; `patch_c50` -2.1 %, `retrieve_c50` -3.4 %). On
  libmicrohttpd without the header patch it was ~1.0 µs (run 2 below: -10.2 %).
- **Per request (`pN`)**: +0.4-0.9 µs on a one-entity request at 1, 10 and 20 policies, the token's
  reception included - `patch_c50` -6.6 / -8.5 / -8.5 %, `retrieve_c50` -5.5 / -6.7 / -5.7 %, `merge_c50`
  -3.0 / -2.9 / -4.8 %, `query_l1_c50` -4.9 / -6.3 / -5.4 %; `create_c50` +0.8-1.6 µs (-5.3 / -9.7 / -9.4 %).
  From 10 policies to 20 adds nothing measurable but on the deletes (below): an `other<K>` is not evaluated
  for a request of another type.
  A request that carries more work - a 20-entity page, a batch, a deep page - pays the same per request:
  `query_c50` 0 to -1.3 %, `batch20_c50` -2.4 to -3.7 %.
- **On the entity (`nN`)**: +1.0-1.5 µs on a one-entity request (`patch_c50` -13.6 / -12.4 / -13.6 %,
  `retrieve_c50` -10.9 / -10.7 / -9.7 %, `merge_c50` -7.1 / -9.7 / -9.2 %), and **0.3-0.7 µs for each further
  entity**: a 20-entity page +8.8-10.5 µs (`query_c50` -9.2 to -10.7 %), 100 entities +34-44 µs
  (`query_l100_c50` -7.8 to -9.7 %), a 20-entry batch update +11.0-14.5 µs (`batch20_c50` -11.0 to -13.9 %) -
  each entry's stored types read before its write. `batch20delete_c50` -14.9 / -17.2 / -19.2 %. 2, 10 or 20
  policies: within 3.0 points of each other on every scenario but `delete_c50` (6.2), `rare_l20_o500_c50`
  (6.1), `patch_c1` (6.0) and `batch20delete_c50` (4.3).
- **Deletes grow with the policies**, per request and on the entity: `delete_c50` -6.0 / -10.1 / -12.3 % at
  `p1` / `p10` / `p20`, -12.3 / -13.7 / -18.6 % at `n2` / `n10` / `n20`; `batch20delete_c50` on the entity
  (above). Not looked into.
- **Notifications**: per request, a notification is not decided - `patch_subs_c50` -1.9 / -3.2 / -3.6 %, the
  PATCH's own decision. On the entity each is decided for its subscription's owner: `patch_subs_c50` -5.6 to
  -5.9 %, `patch_manysubs_c50` -3.5 to -4.5 %, `patch_10subs_c50` (ten per PATCH) -1.9 to -2.8 %.
- **Latency**, `patch_c50` p50 / p99: none 421-431 / 469-493 µs, `pN` 460-471 / 517-540 µs, `nN` 491-499 /
  550-580 µs.
- **The control**: the three no-auth runs are within -3.8 to +3.1 % of their median on every scenario but five,
  where one of them is off: `batch20_c50` (the first, -8.7 %), `query_l100_c50` (the third, -16.4 %),
  `big_r01_l20_c50` (the first, -6.6 %), `big_r1_l20_o1000_c50` (the first, -4.8 %), `big_r01_l20_o200_c50`
  (49 / 49 / 51 requests/s). The median leaves each out.
  `patch_c1`'s -13.4 % at `p10` (-4.6 % at `p1`, -5.2 % at `p20`) and -11.0 % at `n10` (-6.8 / -5.0 %) are the
  single-connection spread
  ([Latency depends on how deep the CPU sleeps](#latency-depends-on-how-deep-the-cpu-sleeps)).
- **Against the sections below** (the same harness, libmicrohttpd without the header patch, before the decision
  skips of [A decision skips what it already knows](#a-decision-skips-what-it-already-knows)): `patch_c50` at
  `p20` -14.0 % (run 2) -> -8.5 %, at `n20` -19.9 % ([the stored types alone](#the-stored-entitys-types-alone-dbdriverentitytypes)) -> -13.6 %.

### Runs 1 and 2 - the token cache and the policy evaluation

| | |
|---|---|
| Builds | Release, libmicrohttpd 1.0.2, every library release (corRest `073a70c`, corNgsild `5839d84`, corDB `643933a`). Run 1: coraine `aab81efe` (main `6aa2eb79` + the harness). Run 2: `f4a59fdc` - the token cache and the policy evaluation below |
| Machine, placement | AMD Ryzen 9 8940HX (16 cores, 32 threads); the broker pinned to one physical core (`PERF_BROKER_CORES=1`), wrk and the notification receiver on the other fifteen; nothing else running |
| Store | corDB in RAM (`--troe none`), perfRun's fixture |
| Token | one RS256 access token as VCVerifier issues it (696 bytes; `test/funcTests/tools/ftJwt.py`), on every request |
| Policies | `pN`: N policies - N-1 `odrl:use` policies for an entity type no request has, and `allowAll` (an `http:path` regex) last: each request is checked against all N before one lets it in |
| `token` | no `--auth`, every request carrying the token anyway: what receiving it costs |
| Runs | run 1 2026-10-12 22:08-23:06, run 2 2026-10-12 23:20 - 2026-10-13 00:35; perfRun's 5 s x 3, the median |

| requests/s, run 1 (before) | none | p1 | p10 | p20 |
|---|---:|---:|---:|---:|
| `query_c50` | 11 649 | 11 398 (-2.2 %) | 11 236 (-3.5 %) | 11 136 (-4.4 %) |
| `query_c200` | 11 446 | 11 382 (-0.6 %) | 11 254 (-1.7 %) | 11 055 (-3.4 %) |
| `query_l1_c50` | 77 037 | 69 760 (-9.4 %) | 68 223 (-11.4 %) | 67 010 (-13.0 %) |
| `query_l100_c50` | 2 470 | 2 512 (+1.7 %) | 2 492 (+0.9 %) | 2 489 (+0.8 %) |
| `retrieve_c50` | 86 223 | 78 832 (-8.6 %) | 76 992 (-10.7 %) | 72 108 (-16.4 %) |
| `patch_c50` | 107 067 | 94 606 (-11.6 %) | 90 260 (-15.7 %) | 85 479 (-20.2 %) |
| `patch_c1` | 45 428 | 45 763 (+0.7 %) | 45 269 (-0.4 %) | 43 937 (-3.3 %) |
| `batch20_c50` | 11 205 | 10 913 (-2.6 %) | 10 444 (-6.8 %) | 10 385 (-7.3 %) |
| `create_c50` | 65 942 | 58 391 (-11.5 %) | 56 179 (-14.8 %) | 54 791 (-16.9 %) |
| `create_c1` | 37 297 | 33 076 (-11.3 %) | 31 806 (-14.7 %) | 30 110 (-19.3 %) |
| `batch20create_c50` | 7 945 | 7 732 (-2.7 %) | 7 366 (-7.3 %) | 7 284 (-8.3 %) |
| `merge_c50` | 79 716 | 72 255 (-9.4 %) | 71 668 (-10.1 %) | 67 705 (-15.1 %) |
| `delete_c50` | 128 599 | 106 072 (-17.5 %) | 103 085 (-19.8 %) | 92 707 (-27.9 %) |
| `batch20delete_c50` | 24 914 | 23 809 (-4.4 %) | 23 398 (-6.1 %) | 22 904 (-8.1 %) |
| `patch_subs_c50` | 22 307 | 21 473 (-3.7 %) | 20 878 (-6.4 %) | 20 275 (-9.1 %) |
| `patch_manysubs_c50` | 20 932 | 20 176 (-3.6 %) | 19 740 (-5.7 %) | 18 743 (-10.5 %) |
| `rare_l20_c50` | 8 065 | 8 032 (-0.4 %) | 7 944 (-1.5 %) | 7 749 (-3.9 %) |
| `rare_l100_c50` | 1 788 | 1 789 (+0.1 %) | 1 782 (-0.3 %) | 1 716 (-4.0 %) |
| `rare_l20_o500_c50` | 584 | 586 (+0.3 %) | 529 (-9.4 %) | 457 (-21.7 %) |
| `big_r1_l20_c50` | 8 168 | 8 124 (-0.5 %) | 7 683 (-5.9 %) | 7 819 (-4.3 %) |
| `big_r1_l20_o1000_c50` | 152 | 150 (-1.3 %) | 144 (-5.3 %) | 121 (-20.4 %) |
| `big_r01_l20_c50` | 2 482 | 2 460 (-0.9 %) | 2 439 (-1.7 %) | 2 134 (-14.0 %) |
| `big_r01_l20_o200_c50` | 51 | 51 (+0.0 %) | 47 (-7.8 %) | 47 (-7.8 %) |

| requests/s, run 2 (after) | none | token | p1 | p10 | p20 |
|---|---:|---:|---:|---:|---:|
| `query_c50` | 11 532 | 11 213 (-2.8 %) | 11 330 (-1.8 %) | 11 307 (-2.0 %) | 11 275 (-2.2 %) |
| `query_c200` | 11 534 | 11 113 (-3.7 %) | 11 058 (-4.1 %) | 11 238 (-2.6 %) | 11 302 (-2.0 %) |
| `query_l1_c50` | 78 407 | 73 242 (-6.6 %) | 72 292 (-7.8 %) | 66 828 (-14.8 %) | 71 030 (-9.4 %) |
| `query_l100_c50` | 2 533 | 2 455 (-3.1 %) | 2 523 (-0.4 %) | 2 512 (-0.8 %) | 2 502 (-1.2 %) |
| `retrieve_c50` | 88 961 | 82 975 (-6.7 %) | 78 138 (-12.2 %) | 81 150 (-8.8 %) | 80 948 (-9.0 %) |
| `patch_c50` | 111 984 | 100 562 (-10.2 %) | 96 862 (-13.5 %) | 95 292 (-14.9 %) | 96 322 (-14.0 %) |
| `patch_c1` | 51 102 | 49 146 (-3.8 %) | 37 542 (-26.5 %) | 41 994 (-17.8 %) | 41 467 (-18.9 %) |
| `batch20_c50` | 11 131 | 11 146 (+0.1 %) | 11 027 (-0.9 %) | 10 970 (-1.4 %) | 10 958 (-1.6 %) |
| `create_c50` | 65 192 | 64 159 (-1.6 %) | 60 928 (-6.5 %) | 57 748 (-11.4 %) | 57 863 (-11.2 %) |
| `create_c1` | 36 120 | 36 009 (-0.3 %) | 34 348 (-4.9 %) | 32 642 (-9.6 %) | 31 808 (-11.9 %) |
| `batch20create_c50` | 7 988 | 7 758 (-2.9 %) | 7 794 (-2.4 %) | 7 821 (-2.1 %) | 7 862 (-1.6 %) |
| `merge_c50` | 82 470 | 76 310 (-7.5 %) | 71 161 (-13.7 %) | 72 851 (-11.7 %) | 74 195 (-10.0 %) |
| `delete_c50` | 128 151 | 114 340 (-10.8 %) | 112 662 (-12.1 %) | 111 023 (-13.4 %) | 106 673 (-16.8 %) |
| `batch20delete_c50` | 24 337 | 23 961 (-1.5 %) | 23 892 (-1.8 %) | 24 372 (+0.1 %) | 23 534 (-3.3 %) |
| `patch_subs_c50` | 22 058 | 21 475 (-2.6 %) | 21 795 (-1.2 %) | 21 214 (-3.8 %) | 20 967 (-4.9 %) |
| `patch_manysubs_c50` | 20 979 | 19 962 (-4.8 %) | 19 128 (-8.8 %) | 19 776 (-5.7 %) | 19 591 (-6.6 %) |
| `patch_10subs_c50` | 3 014 | 2 851 (-5.4 %) | 2 819 (-6.5 %) | 2 966 (-1.6 %) | 2 978 (-1.2 %) |
| `rare_l20_c50` | 8 138 | 7 800 (-4.2 %) | 7 739 (-4.9 %) | 7 962 (-2.2 %) | 7 863 (-3.4 %) |
| `rare_l100_c50` | 1 783 | 1 753 (-1.7 %) | 1 732 (-2.9 %) | 1 782 (-0.1 %) | 1 765 (-1.0 %) |
| `rare_l20_o500_c50` | 577 | 568 (-1.6 %) | 550 (-4.7 %) | 580 (+0.5 %) | 561 (-2.8 %) |
| `big_r1_l20_c50` | 8 233 | 7 941 (-3.5 %) | 8 000 (-2.8 %) | 8 071 (-2.0 %) | 8 011 (-2.7 %) |
| `big_r1_l20_o1000_c50` | 151 | 141 (-6.6 %) | 153 (+1.3 %) | 154 (+2.0 %) | 153 (+1.3 %) |
| `big_r01_l20_c50` | 2 518 | 2 424 (-3.7 %) | 2 456 (-2.5 %) | 2 471 (-1.9 %) | 2 441 (-3.1 %) |
| `big_r01_l20_o200_c50` | 53 | 49 (-7.5 %) | 52 (-1.9 %) | 53 (+0.0 %) | 53 (+0.0 %) |


- **Receiving the token costs about as much as deciding with it.** Without any plugin, a request carrying the
  696-byte header is 0.8-1.0 µs slower: `patch_c50` -10.2 %, `retrieve_c50` -6.7 %, `query_l1_c50` -6.6 %. A
  connector in front of a broker (APISIX) receives the same header.
- **What the decision costs is now about 0.4 µs, whatever the number of policies.** Run 2 at 1, 10 and 20
  policies: `patch_c50` -13.5 / -14.9 / -14.0 %, `retrieve_c50` -12.2 / -8.8 / -9.0 % - within the spread of the
  run. Run 1 grew with the policies: `patch_c50` -11.6 / -15.7 / -20.2 %, `retrieve_c50` -8.6 / -10.7 / -16.4 %.
- **Notifications**: `patch_subs_c50` -4.9 % at 20 policies (run 1 -9.1 %), `patch_manysubs_c50` -6.6 %
  (-10.5 %). Without `--fdscNgsild` a notification is not checked; the PATCH that makes it is.
  `patch_10subs_c50` - each PATCH notifies ten subscribers, 10.1 notifications per PATCH received - -1.2 %.
- **Single connection**: `patch_c1` in run 2 reads -17.8 to -26.5 % for the plugin and -3.8 % for `token`. The
  two builds' `fdsc.so` side by side on one broker setup (corDB, 100 entities, 1 policy; wrk `-t1 -c1`, 10 s
  each, alternating, 2026-10-13 00:50): no `--auth` 48 679, `token` 46 624, run 1's plugin 44 289 / 45 203 /
  45 154, run 2's 45 654 / 45 263 / 46 467 requests/s. The whole layer costs ~1.3 µs on one connection, the
  token's reception ~1 µs of it; the run's `patch_c1` is the single-connection spread
  ([Latency depends on how deep the CPU sleeps](#latency-depends-on-how-deep-the-cpu-sleeps)).

**Where the time went** - `perf record` of the broker (20 policies, PATCH / retrieve / `limit=1` query, 10 s each
at 50 connections), the share of all the broker's cycles, a function with what it calls:

| | PATCH before | after | retrieve before | after | query `limit=1` before | after |
|---|---:|---:|---:|---:|---:|---:|
| `fdscAuthenticate` (the token cache lookup in it: 4.5 / 0.2 %) | 4.44 % | 0.33 % | 4.85 % | 0.43 % | 4.43 % | 0.42 % |
| the decision (`decide`) | 1.31 % | 0.80 % | 2.50 % | 1.75 % | 1.59 % | 1.68 % |
| requests/s while profiled | 79 307 | 86 959 | 85 884 | 96 664 | 79 310 | 83 056 |
| the same without `--auth` | 96 937 | | 111 687 | | 96 427 | |

- **The token cache's key** was an FNV-1a hash of the whole token, byte by byte: ~0.55 µs per request, three
  times the 20 policies. Its key is the token's last 32 bytes - its signature - and its length, 8 bytes at a
  time; `memcmp` of the whole token decides a hit, and only a token whose signature held is ever kept. 64
  locks instead of one: every request of every thread looks a token up.
- **A policy for one entity type** - a rule ANDed into it is `ngsi-ld:entityType odrl:eq "<type>"` - is not
  evaluated for a request of another type; the request's type is read once per decision; a body that is no
  object is not parsed for a `type` it cannot have.
- **The first policy that allows decides**, as OPA's early exit does (a parity fix -
  [How a decision is made](authorization.md#42-how-a-decision-is-made)).
- **What is left**: 4.3-4.7 % of every request - with or without `--auth` - is libmicrohttpd zeroing its 32 kB
  connection pool (`MHD_pool_reset`, `MHD_pool_deallocate`).

### With `--fdscNgsild`

`--fdscNgsild` decides on each entity a request reads or writes: every entity of a query page, every entry of
a batch, every notification, and the stored entity's types before a write. The same harness, configuration
`nN`.

| | |
|---|---|
| Builds | Release, libmicrohttpd 1.0.2. Before: main `a6e67e2f` + the harness. After: `c18b67a4` - the right operands and the type index below |
| Policies | `n20`: 18 `odrl:use` policies for an entity type no request has, an `http:path` policy for `^/ngsi-ld/v1/(subscriptions\|types)`, and last `ngsi-ld:entityType odrl:isAnyOf [Vehicle, Common, Rare, R1, R01]` - perfRun's types |
| Runs | before 2026-10-13 03:07-04:07 (none, `n20`, `p20`, none); after 04:17-05:02 (none, `n20`, none); the second `none` is the control for the position in the run |
| Machine, placement, store, token | as above |

| requests/s | before: none | `n20` | none again | after: none | `n20` | none again |
|---|---:|---:|---:|---:|---:|---:|
| `query_c50` | 11 489 | 6 234 (-45.7 %) | 11 507 (+0.2 %) | 11 527 | 9 831 (-14.7 %) | 11 418 (-0.9 %) |
| `query_c200` | 11 247 | 6 087 (-45.9 %) | 11 577 (+2.9 %) | 11 373 | 9 734 (-14.4 %) | 11 453 (+0.7 %) |
| `query_l1_c50` | 73 872 | 53 406 (-27.7 %) | 77 978 (+5.6 %) | 77 313 | 66 568 (-13.9 %) | 79 647 (+3.0 %) |
| `query_l100_c50` | 2 494 | 1 295 (-48.1 %) | 2 508 (+0.6 %) | 2 482 | 2 139 (-13.8 %) | 2 498 (+0.6 %) |
| `retrieve_c50` | 86 053 | 63 691 (-26.0 %) | 81 532 (-5.3 %) | 87 314 | 73 194 (-16.2 %) | 89 968 (+3.0 %) |
| `patch_c50` | 107 984 | 71 681 (-33.6 %) | 107 807 (-0.2 %) | 105 400 | 83 049 (-21.2 %) | 110 653 (+5.0 %) |
| `patch_c1` | 45 371 | 41 135 (-9.3 %) | 50 058 (+10.3 %) | 49 615 | 40 331 (-18.7 %) | 44 557 (-10.2 %) |
| `batch20_c50` | 11 062 | 4 075 (-63.2 %) | 11 292 (+2.1 %) | 11 029 | 8 397 (-23.9 %) | 10 942 (-0.8 %) |
| `create_c50` | 64 645 | 52 100 (-19.4 %) | 65 722 (+1.7 %) | 65 659 | 55 783 (-15.0 %) | 67 658 (+3.0 %) |
| `create_c1` | 36 776 | 30 940 (-15.9 %) | 33 882 (-7.9 %) | 36 390 | 31 184 (-14.3 %) | 36 894 (+1.4 %) |
| `batch20create_c50` | 7 852 | 6 064 (-22.8 %) | 7 959 (+1.4 %) | 7 755 | 7 305 (-5.8 %) | 7 868 (+1.5 %) |
| `merge_c50` | 81 053 | 60 894 (-24.9 %) | 81 136 (+0.1 %) | 79 686 | 65 025 (-18.4 %) | 83 427 (+4.7 %) |
| `delete_c50` | 123 727 | 81 307 (-34.3 %) | 119 899 (-3.1 %) | 128 808 | 100 182 (-22.2 %) | 128 958 (+0.1 %) |
| `batch20delete_c50` | 25 059 | 10 857 (-56.7 %) | 24 116 (-3.8 %) | 24 694 | 15 683 (-36.5 %) | 24 612 (-0.3 %) |
| `patch_subs_c50` | 22 312 | 19 263 (-13.7 %) | 22 335 (+0.1 %) | 21 949 | 19 812 (-9.7 %) | 22 425 (+2.2 %) |
| `patch_manysubs_c50` | 20 913 | 18 019 (-13.8 %) | 21 081 (+0.8 %) | 20 610 | 18 665 (-9.4 %) | 21 391 (+3.8 %) |
| `patch_10subs_c50` | 3 007 | 2 748 (-8.6 %) | 3 006 (+0.0 %) | 2 899 | 2 831 (-2.3 %) | 3 007 (+3.7 %) |
| `rare_l20_c50` | 8 012 | 4 994 (-37.7 %) | 8 059 (+0.6 %) | 8 048 | 7 151 (-11.1 %) | 8 144 (+1.2 %) |
| `rare_l100_c50` | 1 688 | 1 062 (-37.1 %) | 1 781 (+5.5 %) | 1 741 | 1 569 (-9.9 %) | 1 781 (+2.3 %) |
| `rare_l20_o500_c50` | 570 | 457 (-19.8 %) | 573 (+0.5 %) | 480 | 479 (-0.2 %) | 581 (+21.0 %) |
| `big_r1_l20_c50` | 7 986 | 5 076 (-36.4 %) | 8 145 (+2.0 %) | 8 020 | 7 137 (-11.0 %) | 8 187 (+2.1 %) |
| `big_r1_l20_o1000_c50` | 121 | 120 (-0.8 %) | 151 (+24.8 %) | 121 | 121 (+0.0 %) | 153 (+26.4 %) |
| `big_r01_l20_c50` | 2 260 | 2 063 (-8.7 %) | 2 505 (+10.8 %) | 2 147 | 2 025 (-5.7 %) | 2 495 (+16.2 %) |
| `big_r01_l20_o200_c50` | 46 | 46 (+0.0 %) | 52 (+13.0 %) | 46 | 46 (+0.0 %) | 52 (+13.0 %) |

- **Per entity decided: ~3.7 µs before, ~0.6 µs after.** A 20-entity page (`query_c50`) -45.7 % -> -14.7 %, 100
  entities (`query_l100_c50`) -48.1 % -> -13.8 %, a 20-entry batch update (`batch20_c50`) -63.2 % -> -23.9 %.
- **The cause**: `perf record` of a 20-entity query showed the policies' own types expanded again for every
  entity (15 % of the broker's cycles), and every policy evaluated for every entity. Each rule's right operand
  is now expanded once per policy set, and a policy for other types is skipped on the entity's expanded types.
  Profiled, 20 policies: a 20-entity query 8 824 -> 23 552 requests/s, a batch update 5 107 -> 8 507.
- **Notifications**: `patch_subs_c50` -13.7 % -> -9.7 %, `patch_10subs_c50` (10.2 notifications per PATCH)
  -8.6 % -> -2.3 %.
- **The stored entity's types**: before a write the broker reads the stored entity's types - one per PATCH,
  merge or delete, one per batch entry. In these two runs it retrieved the whole entity for them (below:
  the types alone).
- **The control**: the second no-auth run of each is within -5.3 to +5.6 % of the first, `patch_c1` within
  ±10.3 %. The deep-offset queries (`*_o*`) and `big_r01_l20_c50` read 11-26 % higher in the second, the
  position effect of a long run: their `n20` column is not a cost.

### The stored entity's types alone (`DbDriver.entityTypes`)

The types of the stored entity, for the decision before a write, read alone: corDB clones the entity's
`type` node under the read lock, mongoc finds the document projected to `type` - no copy of the
attributes. The same harness and `n20`; two builds, run in the order none, `n20` (before) and `n20`, none
(after).

| | |
|---|---|
| Builds | Release, libmicrohttpd 1.0.10. Before: main `594222a3`, corDB `643933a`. After: the same + `DbDriver.entityTypes` (coraine `a7053a3f`, corDB `05c2ea5`) |
| Runs | 2026-10-13: before 11:41-12:10, after 12:10-12:40 |
| Machine, placement, store, token, policies | as above (`n20`) |

| requests/s | before: none | `n20` | after: `n20` | none | `n20`, after against before |
|---|---:|---:|---:|---:|---:|
| `query_c50` | 11 385 | 9 933 (-12.8 %) | 9 831 (-15.0 %) | 11 564 | -1.0 % |
| `query_c200` | 11 366 | 9 865 (-13.2 %) | 9 808 (-14.2 %) | 11 428 | -0.6 % |
| `query_l1_c50` | 78 812 | 66 298 (-15.9 %) | 66 657 (-14.6 %) | 78 075 | +0.5 % |
| `query_l100_c50` | 2 496 | 2 156 (-13.6 %) | 2 156 (-13.9 %) | 2 503 | +0.0 % |
| `retrieve_c50` | 88 480 | 73 290 (-17.2 %) | 75 587 (-13.5 %) | 87 403 | +3.1 % |
| `patch_c50` | 111 050 | 82 813 (-25.4 %) | 88 644 (-19.9 %) | 110 632 | +7.0 % |
| `patch_c1` | 50 061 | 42 931 (-14.2 %) | 44 333 (+13.7 %) | 38 979 | +3.3 % |
| `batch20_c50` | 11 232 | 8 204 (-27.0 %) | 9 257 (-16.0 %) | 11 019 | +12.8 % |
| `create_c50` | 63 147 | 54 462 (-13.8 %) | 52 787 (-20.1 %) | 66 083 | -3.1 % |
| `create_c1` | 36 866 | 31 566 (-14.4 %) | 30 845 (+0.8 %) | 30 587 | -2.3 % |
| `batch20create_c50` | 7 824 | 6 092 (-22.1 %) | 6 077 (-21.7 %) | 7 761 | -0.2 % |
| `merge_c50` | 82 200 | 63 663 (-22.6 %) | 67 194 (-15.6 %) | 79 623 | +5.5 % |
| `delete_c50` | 127 502 | 97 650 (-23.4 %) | 100 200 (-24.5 %) | 132 702 | +2.6 % |
| `batch20delete_c50` | 24 990 | 16 323 (-34.7 %) | 19 446 (-22.4 %) | 25 052 | +19.1 % |
| `patch_subs_c50` | 22 166 | 20 376 (-8.1 %) | 20 173 (-8.5 %) | 22 054 | -1.0 % |
| `patch_manysubs_c50` | 21 277 | 19 473 (-8.5 %) | 19 113 (-6.9 %) | 20 519 | -1.8 % |
| `patch_10subs_c50` | 3 024 | 2 944 (-2.6 %) | 2 901 (-2.0 %) | 2 961 | -1.5 % |
| `rare_l20_c50` | 8 092 | 7 226 (-10.7 %) | 7 188 (-9.3 %) | 7 922 | -0.5 % |
| `rare_l100_c50` | 1 764 | 1 585 (-10.1 %) | 1 579 (-9.7 %) | 1 748 | -0.4 % |
| `rare_l20_o500_c50` | 554 | 557 (+0.5 %) | 555 (-1.1 %) | 561 | -0.4 % |
| `big_r1_l20_c50` | 8 099 | 7 181 (-11.3 %) | 7 215 (-10.5 %) | 8 065 | +0.5 % |
| `big_r1_l20_o1000_c50` | 148 | 147 (-0.7 %) | 144 (-0.7 %) | 145 | -2.0 % |
| `big_r01_l20_c50` | 2 416 | 2 325 (-3.8 %) | 2 368 (-1.1 %) | 2 395 | +1.8 % |
| `big_r01_l20_o200_c50` | 50 | 50 (+0.0 %) | 49 (-3.9 %) | 51 | -2.0 % |

- **Writes**: `patch_c50` -25.4 % -> -19.9 %, `merge_c50` -22.6 % -> -15.6 %, `batch20_c50` -27.0 % -> -16.0 %,
  `batch20delete_c50` -34.7 % -> -22.4 %. `delete_c50` -23.4 % -> -24.5 %: within the run-to-run spread.
- **Reads** decided on the stored entity: `retrieve_c50` -17.2 % -> -13.5 %.
- **Unchanged**, as nothing in them reads stored types: the queries, the creates, the notifications.
- **The control**: none after against none before is within -3.6 to +4.6 %, except the single-connection
  scenarios (`patch_c1` -22.1 %, `create_c1` -17.0 %), whose spread between runs is that large
  ([Latency depends on how deep the CPU sleeps](#latency-depends-on-how-deep-the-cpu-sleeps)): their columns
  are not a cost.

**Where a PATCH's 3.1 µs go** (`patch_c50`, one core, 20 policies; before this change): no auth 9.0 µs; the
client's 696-byte token received, no plugin, +1.0 µs (`token`, above); the decision without `--fdscNgsild`
+0.4 µs (`p20` against `token`); `--fdscNgsild` +1.7 µs - the stored types (0.8 µs of it: 12.1 -> 11.3 µs with this change), the
entity's decision, the body's attribute names expanded for `ngsi-ld:attribute`. The token's share is the
client's: its size, sent with every request.

### A decision skips what it already knows

With `--fdscNgsild` a request is decided twice: once before it is run (authorize, the entity not yet known)
and then for each entity. Three policies are not evaluated where their answer is already known:

- **In authorize**, once one policy is pending on the entity, the other policies with an `ngsi-ld:entityType`
  rule are left to the entity's decision.
- **In the entity's decision**, a policy that did not hold in authorize, for the same action and the same
  policy set, is not evaluated again (the first 64 policies of a set).
- **A write of several attributes** is decided once for all of them when no policy of the set reads
  `ngsi-ld:attribute`.

A policy is skipped only where no conflict between policies is possible. The same harness and `n20`; main's
plugin (A) and this change's (C) run in the order none, A, C, C, A.

| | |
|---|---|
| Builds | Release, libmicrohttpd 1.0.10. A: main `caede0cd`. C: `d32a1490` - the plugin alone differs |
| Runs | 2026-10-13: none 15:32, A 15:47, C 16:02, C 16:17, A 16:32-16:47 |
| Machine, placement, store, token, policies | as above (`n20`) |

| requests/s | none | main: `n20` | `n20` | this change: `n20` | `n20` | this change against main |
|---|---:|---:|---:|---:|---:|---:|
| `query_c50` | 11 517 | 9 911 | 9 725 | 10 154 | 10 209 | +3.7 % |
| `query_c200` | 11 499 | 9 786 | 9 704 | 10 076 | 10 168 | +3.9 % |
| `query_l1_c50` | 78 887 | 66 732 | 66 357 | 68 574 | 68 393 | +2.9 % |
| `query_l100_c50` | 2 489 | 2 153 | 2 138 | 2 222 | 2 219 | +3.5 % |
| `retrieve_c50` | 89 821 | 76 187 | 76 290 | 78 490 | 78 139 | +2.7 % |
| `patch_c50` | 108 212 | 89 811 | 88 421 | 92 060 | 92 972 | +3.8 % |
| `patch_c1` | 51 065 | 43 420 | 45 206 | 44 698 | 44 844 | +1.0 % |
| `batch20_c50` | 11 149 | 9 253 | 9 083 | 9 544 | 9 684 | +4.9 % |
| `create_c50` | 67 603 | 53 679 | 52 990 | 56 079 | 57 367 | +6.4 % |
| `create_c1` | 33 878 | 30 341 | 31 496 | 33 013 | 32 293 | +5.6 % |
| `batch20create_c50` | 7 891 | 6 075 | 6 063 | 7 053 | 7 099 | +16.6 % |
| `merge_c50` | 82 952 | 67 501 | 66 552 | 71 332 | 70 061 | +5.5 % |
| `delete_c50` | 130 050 | 101 862 | 101 117 | 105 227 | 97 895 | +0.1 % |
| `batch20delete_c50` | 25 453 | 19 406 | 19 598 | 20 434 | 20 027 | +3.7 % |
| `patch_subs_c50` | 22 030 | 20 467 | 20 511 | 20 629 | 20 768 | +1.0 % |
| `patch_manysubs_c50` | 20 704 | 19 499 | 19 481 | 19 164 | 19 640 | -0.5 % |
| `patch_10subs_c50` | 2 998 | 2 920 | 2 911 | 2 893 | 2 936 | -0.0 % |
| `rare_l20_c50` | 7 996 | 7 182 | 7 155 | 7 401 | 7 269 | +2.3 % |
| `rare_l100_c50` | 1 761 | 1 581 | 1 578 | 1 613 | 1 590 | +1.4 % |
| `rare_l20_o500_c50` | 559 | 555 | 324 | 545 | 547 | +24.2 % |
| `big_r1_l20_c50` | 8 091 | 7 173 | 7 266 | 7 335 | 7 370 | +1.8 % |
| `big_r1_l20_o1000_c50` | 146 | 144 | 147 | 147 | 145 | +0.3 % |
| `big_r01_l20_c50` | 2 439 | 2 325 | 2 340 | 2 368 | 2 371 | +1.6 % |
| `big_r01_l20_o200_c50` | 51 | 49 | 50 | 50 | 50 | +1.0 % |

- **Writes**: `patch_c50` +3.8 %, `merge_c50` +5.5 %, `create_c50` +6.4 %, `batch20_c50` +4.9 %,
  `batch20create_c50` +16.6 % (-23.1 % -> -10.3 % against none). In each, both C runs are above both A runs.
- **Reads**: the queries +2.9 to +3.9 %, `retrieve_c50` +2.7 %.
- **Notifications**: `patch_subs_c50` +1.0 %, `patch_manysubs_c50` -0.5 %, `patch_10subs_c50` -0.0 % - within
  the run-to-run spread.
- **Not a cost or a gain**: `rare_l20_o500_c50` +24.2 % comes from one A run (324 against 555 in the other);
  `delete_c50` +0.1 % has its C runs at 105 227 and 97 895; `patch_c1` and `create_c1` are single-connection
  scenarios ([Latency depends on how deep the CPU sleeps](#latency-depends-on-how-deep-the-cpu-sleeps)).

### The token's size

Every request carries the client's access token, and the broker receives the whole header before anything
reads it. PATCH, 50 connections, the broker on one core, corDB, 100 entities, no `--auth`, 12 s per run, in
the order stock, patched, patched, stock (2026-10-13):

| requests/s | libmicrohttpd 1.0.10 | with `0001-header-value-fastpath.patch` |
|---|---:|---:|
| no `Authorization` header | 108 621 / 107 094 | 107 477 / 107 240 |
| a 696-byte token | 99 366 / 98 127 (-8.5 %, 0.86 µs) | 104 632 / 103 365 (-3.2 %, 0.31 µs) |

In the broker, per PATCH, a 703-byte token against none (`perf stat` on the broker's process, user space;
the kernel's share is the same with and without it), in the order none, token, token, none, 2026-10-13. HTTP:
wrk, `patchAttr.lua`, 8 s per run. cor://: corRequest, 50 connections, 6 s per run - corRequest's load mode
reaches about 15 000 requests/s with the broker's core 18 % busy, so for cor:// only the count per request is
a measure, not requests/s:

| per PATCH | no header: instructions / cycles | the token: instructions / cycles |
|---|---:|---:|
| HTTP, libmicrohttpd 1.0.10 | 51 434 / 21 620, 51 327 / 21 264 | 66 425 / 25 565, 66 548 / 26 077 (+15 100 / +4 380) |
| HTTP, with the patch | 51 084 / 21 874, 50 611 / 20 852 | 54 459 / 23 082, 54 430 / 22 778 (+3 600 / +1 570) |
| cor:// | 32 010 / 18 108, 32 147 / 18 235 | 33 010 / 18 286, 33 011 / 17 844 (+940 / -110) |

- **cor://**: the token is one length-prefixed string of the request's tree (corTree's binary codec); its
  per-connection tables hold names and the namespaces of IRIs, and a token is neither, so it travels whole with every
  request. Its decoding costs 940 instructions, its cycles are within the spread of the runs.

- **Where**: libmicrohttpd's header parser, `get_req_headers` - 2.0 % of the broker's cycles without the
  header, 9.2 % with it (`perf record`, 10 s). It reads a value one character at a time; corRest's copy of
  the header does not show.
- **The patch** (corLibs `tools/mhd/patches/1.0.10/`, applied by `mhdBuild.sh` in every build of the stack)
  skips a value's plain characters in a compare-only loop; line ends, folding, bare CR / LF and the client
  discipline levels take libmicrohttpd's path unchanged. libmicrohttpd's `make check`: 180 of 180.
- **The size is the client's**: VCVerifier's token for one `UserCredential` is 696 bytes; a credential with
  more claims makes it larger. With `--auth` the broker says so once, on a thread's first request or every
  1024th, when an `Authorization` header is over 512 bytes:

```
W: requests carry a 696-byte Authorization header - received with every request, its size is a cost of each (doc/performance.md, "The token's size")
```

## The decision in the broker, or in a gateway

The same access decision - the Data Space Connector's tokens and ODRL policies, 20 of them - made in four
places, and the throughput a client gets:

| configuration | where the decision is made |
|---|---|
| none | nowhere: the broker alone, no token |
| `--auth fdsc` | in the broker (`fdsc.so`) |
| APISIX | nowhere: APISIX 3.17.0 in front, no plugin - the cost of the hop alone |
| APISIX + OPA | the connector's chain today: `openid-connect` (the token), odrl-authorization 2.4.3's `opa` plugin, OPA 1.11.0 with the Rego [odrl-pap](https://github.com/SEAMWARE/odrl-pap) 1.7.1 makes of the policies |
| APISIX + ngsild-pep | in APISIX, by `fdsc.so`'s own code: the plugin `ngsild-pep` ([corApisixNgsild](https://github.com/SEAMWARE/corApisixNgsild)) - a Lua plugin calling, through LuaJIT's FFI, a library built from `fdsc.so`'s sources. On the 64 cases of `test/fdscDiff` at the coraine commit its CI pins (`7239cf37`) it decides as `--auth fdsc` does, case for case |

Behind every gateway the broker runs without `--auth`. The same token (RS256, a `UserCredential`) and the same
policies everywhere (`perfPolicies.py 20`: 19 that never match, then one that allows all).

**Placement** (`test/perf/gatewayCompare.sh`): the broker on physical core 0, APISIX - one nginx worker - on core
1, OPA - `GOMAXPROCS=1` - on core 2, and the load generator on cores 3-15, in **every** configuration
(`PERF_RESERVED_CORES=2`), so that the load is the same whether a gateway runs or not. APISIX's
`client_body_buffer_size` is `1m` (below). AMD Ryzen 9 8940HX, 16 cores / 32 threads; coraine main of
2026-10-13 (release build, corDB in RAM, `--troe none`), libmicrohttpd 1.0.10; 2026-10-14, Chrome closed.

### Requests a second

`perfRun.sh`'s scenarios, the median of its repeats; the percentages against the mean of the two runs without
security (they differ by 4 % or less in 20 of the 24 scenarios; `patch_c1`, `delete_c50` and
`rare_l20_o500_c50` by 11-19 %):

| scenario | none | `--auth fdsc` | APISIX | APISIX + OPA | APISIX + ngsild-pep | none again | fdsc | APISIX | ngsild-pep | OPA |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `query_c50` | 11 293 | 11 045 | 11 142 | 2 854 | 11 051 | 11 412 | -2.7 % | -1.9 % | -2.7 % | -74.9 % |
| `query_c200` | 11 333 | 11 170 | 11 082 | 2 710 | 11 042 | 11 363 | -1.6 % | -2.3 % | -2.7 % | -76.1 % |
| `query_l1_c50` | 70 155 | 64 203 | 40 309 | 2 694 | 32 727 | 68 473 | -7.4 % | -41.8 % | -52.8 % | -96.1 % |
| `query_l100_c50` | 2 453 | 2 469 | 2 486 | 2 312 | 2 449 | 2 496 | -0.2 % | +0.5 % | -1.0 % | -6.6 % |
| `retrieve_c50` | 78 619 | 72 615 | 36 338 | 2 890 | 34 174 | 77 761 | -7.1 % | -53.5 % | -56.3 % | -96.3 % |
| `patch_c50` | 106 194 | 98 928 | 40 724 | 2 069 | 34 185 | 110 952 | -8.9 % | -62.5 % | -68.5 % | -98.1 % |
| `patch_c1` | 44 270 | 37 388 | 18 256 | 1 668 | 16 302 | 39 555 | -10.8 % | -56.4 % | -61.1 % | -96.0 % |
| `batch20_c50` | 10 256 | 10 053 | 11 003 | 1 720 | 11 103 | 10 025 | -0.9 % | +8.5 % | +9.5 % | -83.0 % |
| `create_c50` | 57 819 | 53 374 | 37 976 | 2 780 | 28 546 | 59 612 | -9.1 % | -35.3 % | -51.4 % | -95.3 % |
| `create_c1` | 30 038 | 28 043 | 16 240 | 1 868 | 13 699 | 30 671 | -7.6 % | -46.5 % | -54.9 % | -93.8 % |
| `batch20create_c50` | 7 628 | 7 364 | 7 811 | 1 200 | 7 722 | 7 794 | -4.5 % | +1.3 % | +0.1 % | -84.4 % |
| `merge_c50` | 72 385 | 68 367 | 40 873 | 2 996 | 33 357 | 71 934 | -5.3 % | -43.4 % | -53.8 % | -95.8 % |
| `delete_c50` | 105 001 | 113 491 | 39 030 | 3 057 | 31 466 | 118 911 | +1.4 % | -65.1 % | -71.9 % | -97.3 % |
| `batch20delete_c50` | 24 659 | 23 596 | 25 042 | 2 048 | 24 255 | 24 725 | -4.4 % | +1.4 % | -1.8 % | -91.7 % |
| `patch_subs_c50` | 21 139 | 20 192 | 21 262 | 2 158 | 20 729 | 21 606 | -5.5 % | -0.5 % | -3.0 % | -89.9 % |
| `patch_manysubs_c50` | 19 968 | 18 818 | 19 336 | 2 134 | 19 808 | 20 106 | -6.1 % | -3.5 % | -1.1 % | -89.3 % |
| `patch_10subs_c50` | 2 941 | 2 805 | 2 886 | 2 077 | 2 920 | 3 003 | -5.6 % | -2.9 % | -1.7 % | -30.1 % |
| `rare_l20_c50` | 7 865 | 7 767 | 7 728 | 3 006 | 7 653 | 8 015 | -2.2 % | -2.7 % | -3.6 % | -62.1 % |
| `rare_l100_c50` | 1 713 | 1 732 | 1 726 | 1 681 | 1 732 | 1 768 | -0.5 % | -0.8 % | -0.5 % | -3.4 % |
| `rare_l20_o500_c50` | 476 | 470 | 419 | 432 | 431 | 573 | -10.4 % | -20.1 % | -17.8 % | -17.6 % |
| `big_r1_l20_c50` | 8 039 | 7 906 | 7 721 | 3 065 | 7 759 | 8 123 | -2.2 % | -4.5 % | -4.0 % | -62.1 % |
| `big_r1_l20_o1000_c50` | 144 | 146 | 118 | 135 | 127 | 149 | -0.3 % | -19.5 % | -13.3 % | -7.8 % |
| `big_r01_l20_c50` | 2 459 | 2 405 | 2 422 | 2 180 | 2 357 | 2 453 | -2.1 % | -1.4 % | -4.0 % | -11.2 % |
| `big_r01_l20_o200_c50` | 49 | 50 | 45 | 49 | 44 | 50 | +1.0 % | -9.1 % | -11.1 % | -1.0 % |

### What a request costs each process

CPU time a request, in µs: 10 s of retrieve, then 10 s of PATCH, at 50 connections, each process's core read
from `/proc/stat` (busy ticks over requests; `<label>-cost.json`):

| configuration | retrieve: broker | APISIX | OPA | PATCH: broker | APISIX | OPA |
|---|---:|---:|---:|---:|---:|---:|
| none | 7.5 | - | - | 8.8 | - | - |
| `--auth fdsc` | 8.3 | - | - | 9.4 | - | - |
| APISIX | 7.0 | 24.9 | - | 8.8 | 25.5 | - |
| APISIX + OPA | 12.7 | 212.7 | 324.2 | 16.3 | 208.8 | 495.8 |
| APISIX + ngsild-pep | 4.6 | 29.3 | - | 7.4 | 29.3 | - |
| none again | 7.4 | - | - | 8.5 | - | - |

- **In the broker** the decision costs 0.8 µs on a retrieve and 0.6-0.9 µs on a PATCH: -5 % to -11 % of the
  requests a second on the one-entity scenarios (`delete_c50` within the spread), up to -7 % on queries and
  notifications.
- **APISIX alone** takes 25 µs a request on its core: one nginx worker carries about 40 000 requests a second,
  so every scenario the broker serves faster than that (retrieve, PATCH, create, merge, delete) is APISIX's
  number behind it, -35 % to -65 %. The scenarios slower than that (queries, batches, notifications) do not
  notice it.
- **ngsild-pep** adds 4 µs to APISIX's 25: the decision in the gateway costs about what it costs in the
  broker, plus the request's way across the FFI line. Up to -25 % against APISIX alone where APISIX is the
  limit - the most on a create, whose body the plugin hands over; within the spread elsewhere.
- **APISIX + OPA** takes 210 µs in APISIX and 320-500 µs in OPA a request: about 3 000 requests a second
  at most, -75 % to -98 % where the broker is faster than that.
- The broker's own µs a request behind a gateway (4.6-16 µs) moves with the rate it is offered: at the lower
  rates a request finds it less warm.

### Run 1 - the request body on disk

The first run (08:14-09:50) used APISIX's default `client_body_buffer_size`. A body larger than it (8 KiB here)
goes to a temporary file, and the request takes 41 ms more: `batch20create_c50` (20 entities, 11.6 KB a
request) was 1 153 a second through APISIX, with and without a plugin (p50 41.0 ms), against 7 764 direct.
One 11.6 KB create: 0.5 ms direct, 41 ms through APISIX, 0.7 ms with `client_body_buffer_size 1m`. With
`1m` (run 3): 7 811 through APISIX, 7 722 with ngsild-pep. Every other scenario of run 1 was within the
spread of run 3. Run 2 is not used: another APISIX was listening on the same port (host networking) and took
part of the load; `gatewayCompare.sh` now refuses to start when a port it needs is in use.

### The connector's provider side, and coraine - memory, start, store

`test/perf/footprint/footprint.sh`: the Data Space Connector's provider side as its chart's examples deploy it
(data-space-connector 10.10.3: decentralized-iam 2.1.27), Scorpio as the NGSI-LD data service - which the
connector's provider guide places outside the connector, and in production on a broker of its own, one container each, the images its charts pin, the
settings their READMEs give - against one coraine with `--auth fdsc` (the same 20 policies) and the PAP's
`/policy` API (`--policyApiPort`), on its durable store (corDB `--dbDir`). Memory: each process's cgroup,
`memory.current` less `inactive_file` (what `docker stats` shows), 60 s after start, and 30 s after the same
10 000 entities (100 batch creates of 100, five attributes each) went in through the broker's own port.

| process | what it is | idle MiB | 10 000 entities MiB |
|---|---|---:|---:|
| Scorpio java-4.1.10 | the broker | 303 | 1 157 |
| PostGIS 16 | Scorpio's database | 46 | 166 |
| Trusted Issuers List 0.9.1 | which issuers may issue which credentials | 297 | 295 |
| Credentials Config Service 3.4.2 | which credentials each service takes | 365 | 364 |
| VCVerifier 6.23.1 | the token: a credential exchanged for a JWT | 14 | 14 |
| APISIX 3.17.0 (one worker) | the gateway | 31 | 32 |
| OPA 1.11.0 | the policy decision | 19 | 16 |
| odrl-pap 1.7.1 | the policies' API | 26 | 26 |
| PostgreSQL 16 | the TIL's, the CCS's and the PAP's database | 69 | 71 |
| **the connector's provider side** | 9 processes | **1 170** | **2 141** |
| **coraine** | the broker, `--auth fdsc`, `/policy` | **31** | **67** |

| | the connector's provider side | coraine |
|---|---:|---:|
| until the broker answers | 4.2 s (from `docker compose up`, images pulled) | 0.54 s |
| 10 000 entities in | 3.3 s | 0.2 s |
| the store on disk | 126 MiB (`pg_database_size`) | 8.0 MiB (`du` of `--dbDir`) |
| images, compressed (amd64) | Scorpio 281, APISIX 132, OPA 35, odrl-pap 44, VCVerifier 14, CCS 191, TIL 168, PostgreSQL 16-alpine 111, PostGIS 203 MiB | 69 MiB (`quay.io/seamware/coraine:0.5.0`) |

coraine does not do what the TIL, the CCS and VCVerifier do (rows 3-5), and the PostgreSQL of row 9 stays for
the TIL: the 31 and 67 MiB stand for the broker and its database, the gateway, OPA and the PAP, rows 1-2 and
6-8 (425 MiB idle, 1 397 MiB with the entities). Since data-space-connector 10.11.0 the CCS is disabled by
default and VCVerifier serves its API. APISIX's chart default is one nginx worker a core (`worker_processes: auto`); with one worker the
row is the smallest it can be.

## An open question: `--connectionPoolSize`

On 2026-09-16, `corHttp` measured **slower than libmicrohttpd on one core**: 5 901 req/s
against 6 588 ([the 2026-09-16 numbers](history/performance.md)). The loop count is
not the cause — `--httpLoops` resolves to 1 on one core, which is correct, and
forcing 2 or 4 there makes it worse, as it should.

The suspect is `--connectionPoolSize` - 32 then, and 32 now for a store that may wait (0, the
default, is the number of cores only for one that never waits: [Configuration](installation.md#core-options)).
Swept on one core, built-in server, 2026-09-16:

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
