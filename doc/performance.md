# Performance and footprint

Every number on this page was measured on one machine on 2026-09-15, from
release builds of the commit it ships with, and each section says how. Nothing
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
| Beside it | MongoDB 4.4 (container, replica set) · PostgreSQL 18 + TimescaleDB 2.25 (host) |

Four builds are measured throughout — the two axes that change what a coraine
process is made of:

- **HTTP server** — `COR_HTTP_SERVER=builtin`, the epoll server in `corHttp`,
  with no external dependency at all; or `COR_HTTP_SERVER=mhd`, the external
  libmicrohttpd.
- **Current-state DB** — `--database corDB`, entities in this process's own RAM;
  or `--database mongoc`, entities in a MongoDB server.

TRoE is `none` in every row, and the `admin` API plugin (23 KiB) is not loaded.

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

What the three columns are made of:

- **coraine's own code is all of it.** The cor and k libraries are static
  archives, whole-archived into the binary, so that 963 KiB is not a `main`
  calling out to something else — it *is* `corNgsild` (341 KiB of NGSI-LD rules),
  the broker's own service routines (361 KiB), `corRest` (50 KiB), `kargs`
  (45 KiB), `corJsonld` (31 KiB), `kjson` (31 KiB), `corHttp` (12 KiB), and
  `kprom`/`kalloc`/`ktrace`/`kbase`/`corPlugin`/`khash` under 6 KiB each. Every
  line of it is in this project's repositories. Add the DB plugin (`corDB.so`
  39 KiB or `mongoc.so` 116 KiB) and `none.so` (14 KiB) and that is the whole
  broker.
- **The three added libraries in the first row** are GEOS (`libgeos` +
  `libgeos_c`, 3.17 MiB — geo-queries) and `libmosquitto` (106 KiB — MQTT
  notifications). GEOS is now the largest single thing coraine puts on a machine,
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

| Configuration | Idle RSS | Private dirty | Serving 50 connections | 100 000 entities | …and serving |
|---------------|---------:|--------------:|-----------------------:|-----------------:|-------------:|
| libmicrohttpd + `corDB` | 13.1 MiB | 3.9 MiB | 31.9 MiB | 407 MiB | 452 MiB |
| libmicrohttpd + `mongoc` | 19.6 MiB | 5.0 MiB | 34.7 MiB | 202 MiB | 203 MiB |
| `corHttp` + `corDB` | 17.5 MiB | 10.6 MiB | 36.2 MiB | 372 MiB | 413 MiB |
| `corHttp` + `mongoc` | 24.0 MiB | 11.7 MiB | 39.2 MiB | 117 MiB | 115 MiB |

The built-in server starts ~4 MiB heavier, and deliberately: it allocates its
connection pool — 1024 slots, 16 KiB of buffer each — at start-up, so no request
ever calls `malloc` for its own machinery. libmicrohttpd reserves far more than
that, but virtually: thread-per-connection stacks put its `VmSize` at 2.7 GiB
against corHttp's 498 MiB, almost none of it touched. The trade shows up again in
the last two columns — loading 100 000 entities over MongoDB grows the
libmicrohttpd build by 182 MiB and the pooled one by 78.

With **`corDB` an entity is RAM**, and that is the number to size a box with:
**~3.5 KiB resident per five-attribute entity** (3.9 KiB on the libmicrohttpd
build), so 100 000 entities is ~400 MiB and a million is ~3.5 GiB. With `mongoc`
the entities are MongoDB's problem and the broker stays flat — but then MongoDB
is on the machine, which is the next table.

## What else has to be running

| `--database` / `--troe` | Also required | What that costs |
|-------------------------|---------------|-----------------|
| `corDB` / `none` | **nothing** | — |
| `corDB` / `ramdb` | nothing | temporal history in the same process |
| `mongoc` | a MongoDB server | 1.02–1.15 GiB resident here, and WiredTiger's default cache is half of (RAM − 1 GiB): on this 60 GiB host mongod is entitled to ~30 GiB. Image: `mongo:4.4` 594 MB, `mongo:8` 1.3 GB |
| `timescale` (TRoE) | a PostgreSQL + TimescaleDB server | 35 MB of packages; 128 MiB of shared buffers by default, 313 MiB summed across its 10 processes here |

Which is the point of the first row. A `corHttp` + `corDB` + `none` deployment is
**one process, 18 MiB, 4.3 MiB of new files on disk, and no socket to anything
else** — and the comparison that matters is not coraine's libraries against
another broker's, it is one process against a broker plus a database server plus
a time-series database server.

## Start-up

From `exec` to a served HTTP response, median of nine:

| Configuration | Ready in |
|---------------|---------:|
| libmicrohttpd + `corDB` | **10.3 ms** |
| `corHttp` + `corDB` | **12.8 ms** |
| libmicrohttpd + `mongoc` | **18.9 ms** |
| `corHttp` + `mongoc` | **21.2 ms** |

corHttp's extra 2.5 ms is the connection pool it allocates up front; mongoc's
extra 8 ms is the handshake with the database server. All four are fast enough
that the broker is not something you keep warm — it is something you start.
Scale-to-zero, per-test instances, one broker per tenant on a gateway: all of
them stop being awkward at 13 ms and 18 MiB.

## Throughput — per core

Throughput per core is the figure that transfers to other hardware; a total is
mostly a statement about how many cores were in the machine. The broker is pinned
to *n* physical cores, `wrk -t8 -c50` runs on physical cores the broker was never
given, MongoDB (where used) on four more of its own. Fixture: 100 preloaded
five-attribute entities (~550 B each), `GET /entities?type=Vehicle&limit=20`,
~11 KB per response, best of three 5 s runs.

**Requests/s per broker core:**

| Configuration | 1 core | 2 cores | 4 cores | 8 cores |
|---------------|-------:|--------:|--------:|--------:|
| libmicrohttpd + `corDB` | **6 300** | 6 047 | 6 483 | 5 841 |
| `corHttp` + `corDB` | **6 623** | 5 366 | 4 937 | 3 760 |
| libmicrohttpd + `mongoc` | **5 036** | 5 178 | 4 636 | — |
| `corHttp` + `mongoc` | **4 680** | 4 547 | 4 274 | — |

At 20 entities per response, 6 300 req/s per core is **126 000 entities/s per
core**. One core of a laptop CPU, going through MongoDB, still serves 5 000
NGSI-LD queries a second.

**The totals and the tail latency behind those figures:**

| Configuration | 4 cores | p99 | 8 cores | p99 |
|---------------|--------:|----:|--------:|----:|
| libmicrohttpd + `corDB` | 25 935 | 25.6 ms | 46 729 | 10.5 ms |
| `corHttp` + `corDB` | 19 751 | 9.0 ms | 30 082 | **2.6 ms** |
| libmicrohttpd + `mongoc` | 18 546 | 4.5 ms | — | — |
| `corHttp` + `mongoc` | 17 099 | 5.8 ms | — | — |

The two HTTP servers are not the same trade. libmicrohttpd's thread-per-
connection reaches a higher peak and keeps scaling — 7.4× on 8 cores — but
carries a long tail. corHttp is faster on one core, holds p99 three to four times
lower at every width, and gives up throughput as cores are added: 4.5× on 8
cores. Its accept loop is a single thread and past four cores that is what is
being measured. Neither is "the fast one": pick the peak or pick the tail.

> Numbers are from one machine and one shape of request — reproduce them on yours
> before quoting them. What travels is the shape: sub-millisecond work per
> request, ~6 000 requests per second per core, and a broker that saturates
> cleanly rather than collapsing. The additional clients wait; they do not make
> the broker slower at serving the ones already there.

## Does it use the cores you give it?

Throughput per core says what a core is worth. This says whether buying more of
them works, and the answer differs by HTTP server. Same query and fixture,
`--database corDB` — deliberately, not for convenience. With `mongoc`, mongod
takes cores of its own on the same machine, so the curve would describe *a broker
and a database sharing one host* and would bend where MongoDB stopped scaling
rather than where the broker did. Both are real questions. This one is "does the
broker use the cores it is given", so the storage engine has to be out of the
answer — an in-memory backend does that, and leaves request parsing, matching,
rendering and the HTTP layer as the only things being measured.

| Cores | libmicrohttpd | vs 1 core | `corHttp` | vs 1 core |
|------:|--------------:|----------:|----------:|----------:|
| 1 | 6 300 | — | 6 623 | — |
| 2 | 12 095 | 1.92× | 10 732 | 1.62× |
| 4 | 25 935 | 4.12× | 19 751 | 2.98× |
| 8 | 46 729 | **7.42×** | 30 082 | 4.54× |

Eight cores do 7.4 times the work of one on the libmicrohttpd build — a bigger
box is worth buying, and a smaller one costs you only what you took away. (The
4-core point is slightly superlinear; that is clock boost, not magic, and it is
why the ratios are quoted rather than a headline efficiency.) The built-in server
does not have that property yet and the table says so. Repeat runs vary by a few
percent; the ratios do not.

> ⚠️ Two limits on that table, both from running `wrk` on the same machine. It
> competes for cache and memory bandwidth, so the broker is if anything
> understated. And it caps the sweep at half the cores: something has to drive
> the load. A first attempt that ignored SMT — load generator on the *siblings*
> of the broker's own cores — produced a neat regression at 16 cores that was
> pure measurement artefact. Anything beyond that needs a second machine, and a
> link faster than the ~4 Gbit/s these responses already push.



---

## Reproducing this

- **Throughput and core scaling** — [`test/perf/coreScale.sh`](https://github.com/SEAMWARE/coraine/blob/main/test/perf/coreScale.sh)
  reads the CPU topology rather than assuming it, and pins the load generator off
  the broker's own physical cores. [`test/perf/perfRun.sh`](https://github.com/SEAMWARE/coraine/blob/main/test/perf/perfRun.sh)
  measures the fixed request shapes and prints one JSON object per run.
- **Size** — a stripped release build, plus the transitive `ldd` closure of the
  binary and the loaded plugins, minus everything a bare `ubuntu:26.04` already
  carries.
- **RAM** — `/proc/<pid>/smaps_rollup`, idle and under `wrk -t8 -c50`.
- **Start-up** — `exec` to a served HTTP response, median of nine.

The two build axes are `-DCOR_HTTP_SERVER=builtin|mhd` and `--database
corDB|mongoc`; the reference build is ICU-free
(`-DCOR_FEATURE_ICU_COLLATION=OFF`, and `COR_WITH_ICU=0` for `corNgsild`).
[Building from source](building.md) has the rest of the switches.
