# Performance - history

What was tried for speed and dropped, regressions and how they were found, and the before-and-after
of the changes behind today's numbers. The numbers as they are now are in
[Performance and footprint](../performance.md). Newest first.

## 2026-10-05 - batch update as what changed; `--dbCompress`; every table from one clean run

Every table of the performance page from one run (12:29-14:48): Chrome closed - a browser that spins
up beside a measurement is noise nobody sees - the databases on the host network, PGO release.

- **Batch update gives the store what changed**, not the whole entity (coraine#240): corDB on disk
  27 630 → 34 762 (+25.8 %), with history 19 060 → 23 239 (+21.9 %), coraine + MongoDB 2 662 → 3 130
  (+17.6 %, a `$set` of the attributes instead of a document rewrite). The rows it does not touch
  moved −3.4 % to +2.5 %.
- **The way there:** the change reports applied under corDB's write lock first - the clones, the
  record's encoding, a walk of the store for every entity - and batch update lost 13-15 % against the
  whole-entity path, which prepared all of that before the lock. Prepared before the lock, with the
  id index, it is the +22-26 % above.
- **`--dbCompress`** (corDB#8): within the noise on every row but batch create with history, −7.4 %.
- A first measurement of the change-report path ran partly while Chrome was open; its rows the
  change does not touch came out 2-7 % low. Not used.

#### The eight-core tables before (2026-10-05 01:04-03:17 and 2026-10-04 23:46-00:52: corDB with ATTRS_PUT, batch update still the whole entity)

**Requests/s:**

| | coraine + MongoDB | coraine + MongoDB + TimescaleDB | coraine, corDB on disk | coraine, corDB on disk + history | coraine, ramDB | Orion-LD + MongoDB | Orion-LD + MongoDB + PostgreSQL |
|---|---:|---:|---:|---:|---:|---:|---:|
| create | 43 660 | 4 732 | 121 450 | 84 417 | 204 312 | 6 466 | 4 991 |
| create, 1 connection | 7 985 | 747 | 28 433 | 22 846 | 31 280 | 4 175 | 1 114 |
| batch create (20) | 12 236 | 396 | 13 521 | 7 680 | 51 660 | 3 571 | 1 770 |
| merge | 26 595 | 7 455 | 103 106 | 82 574 | 131 220 | 6 381 | 5 319 |
| `PATCH` | 17 360 | 8 516 | 144 465 | 119 962 | 183 151 | 6 500 | 5 374 |
| `PATCH`, 1 connection | 3 859 | 897 | 38 266 | 36 057 | 39 777 | 3 500 | 974 |
| batch update (20) | 2 662 | 767 | 27 630 | 19 060 | 38 396 | 2 482 | 2 081 |
| `DELETE` | 28 575 | 4 826 | 211 939 | 189 868 | 225 379 | 18 494 | 9 457 |
| batch delete (20) | 7 346 | 2 178 | 66 526 | 49 863 | 66 600 | 3 138 | 2 489 |
| `GET /entities/{id}` | 53 102 | 54 024 | 470 714 | 496 282 | 467 884 | 31 199 | 30 918 |
| query, `limit=20` | 22 107 | 23 567 | 76 563 | 80 441 | 77 306 | 5 521 | 5 217 |

**Latency, p50 / p95 / p99, ms:**

| | coraine + MongoDB | coraine + MongoDB + TimescaleDB | coraine, corDB on disk | coraine, corDB on disk + history | coraine, ramDB | Orion-LD + MongoDB | Orion-LD + MongoDB + PostgreSQL |
|---|---:|---:|---:|---:|---:|---:|---:|
| create | 1.08 / 1.46 / 2.37 | 9.45 / 18.6 / 31.2 | 0.31 / 5.94 / 9.71 | 0.43 / 15.7 / 33.6 | 0.22 / 0.38 / 0.47 | 6.89 / 14.6 / 21.6 | 9.44 / 18.1 / 24.7 |
| create, 1 connection | 0.12 / 0.18 / 0.20 | 1.32 / 1.45 / 23.4 | 0.03 / 0.04 / 1.02 | 0.04 / 0.06 / 1.30 | 0.03 / 0.03 / 0.04 | 0.23 / 0.28 / 0.35 | 0.90 / 0.98 / 1.14 |
| batch create (20) | 3.62 / 8.26 / 22.0 | 109 / 241 / 326 | 2.34 / 10.6 / 14.8 | 4.14 / 27.9 / 98.2 | 0.83 / 1.89 / 2.82 | 12.6 / 27.8 / 38.9 | 26.4 / 47.6 / 63.7 |
| merge | 1.80 / 2.20 / 2.55 | 5.68 / 13.2 / 16.2 | 0.44 / 0.80 / 1.01 | 0.56 / 1.06 / 1.30 | 0.32 / 0.82 / 1.02 | 7.36 / 11.9 / 15.1 | 8.88 / 16.5 / 22.4 |
| `PATCH` | 2.76 / 3.24 / 3.51 | 5.01 / 10.8 / 13.4 | 0.31 / 0.64 / 0.79 | 0.37 / 0.78 / 0.99 | 0.26 / 0.40 / 0.47 | 7.27 / 11.8 / 14.7 | 8.76 / 17.3 / 23.8 |
| `PATCH`, 1 connection | 0.24 / 0.29 / 0.34 | 1.10 / 1.21 / 1.28 | 0.02 / 0.03 / 0.03 | 0.03 / 0.03 / 0.04 | 0.02 / 0.02 / 0.03 | 0.28 / 0.29 / 0.31 | 1.03 / 1.09 / 1.20 |
| batch update (20) | 17.8 / 21.3 / 23.0 | 56.7 / 133 / 162 | 1.60 / 2.70 / 3.23 | 2.29 / 4.12 / 5.01 | 1.23 / 1.47 / 1.62 | 18.6 / 32.3 / 40.6 | 21.9 / 43.4 / 56.7 |
| `DELETE` | 1.64 / 1.97 / 2.44 | 7.81 / 19.0 / 24.1 | 0.21 / 0.35 / 0.42 | 0.19 / 0.42 / 0.50 | 0.18 / 0.32 / 0.38 | 2.50 / 4.06 / 5.70 | 5.00 / 8.90 / 12.2 |
| batch delete (20) | 5.70 / 12.6 / 16.0 | 14.2 / 50.3 / 80.2 | 0.67 / 0.85 / 0.97 | 0.92 / 1.10 / 1.19 | 0.59 / 0.83 / 0.98 | 14.0 / 28.1 / 35.9 | 16.1 / 36.4 / 66.6 |
| `GET /entities/{id}` | 0.90 / 1.16 / 1.37 | 0.89 / 1.14 / 1.36 | 0.10 / 0.15 / 0.17 | 0.09 / 0.13 / 0.15 | 0.10 / 0.14 / 0.91 | 1.49 / 2.87 / 4.26 | 1.51 / 2.86 / 4.17 |
| query, `limit=20` | 2.08 / 3.51 / 4.34 | 2.02 / 2.88 / 3.56 | 0.64 / 0.84 / 0.85 | 0.59 / 0.89 / 0.92 | 0.52 / 1.54 / 1.60 | 7.95 / 17.5 / 22.3 | 8.48 / 18.8 / 25.0 |

Eight physical cores for the whole deployment - the broker, and the databases it needs (mongod 8.2 and
PostgreSQL 16 + TimescaleDB as containers on the host network - no port mapping - pinned to the same
cores); the load generator on the other eight, standing in for clients. 50 connections unless said,
median of 3 × 5 s, `test/perf/perfRun.sh`, AMD Ryzen 9 8940HX, 2026-10-04/05. coraine a PGO release,
corDB on disk with `--dbDir`, its log memory-mapped: a write's record is in the kernel before the
response, so a broker that dies loses nothing it acknowledged, and a machine that dies at most the
last 100 ms - MongoDB's default. Orion-LD 1.15.0-next, its release build (`-O3`), `-mongocOnly`.

- **History is where it shows.** In a database server it costs a write 51-97 % (coraine + MongoDB +
  TimescaleDB: create 43 660 → 4 732; Orion-LD + PostgreSQL 16-73 %); in corDB 6-43 %, and coraine with corDB
  and its history outruns MongoDB without any on every write but batch create (20) - and MongoDB + TimescaleDB
  11-40×.
- **Against Orion-LD with its history**: coraine with corDB and its history is 4.3-37× on every row,
  reads included.
- **Persistence costs little:** ramDB, no disk at all, is 1.0-1.7× corDB on disk; 3.8× on batch
  create, which grows the store fastest and so snapshots most.
- **Reads** do not touch history in any configuration. corDB answers a retrieve 8.9× MongoDB's rate,
  470 714 a second at a p99 of 0.17 ms.
- **The tails of a write** follow the throughput: p99 of a batch update 5.01 ms (corDB + history) against
  162 ms (MongoDB + TimescaleDB).

#### What persistence costs, before (2026-10-05 03:28-04:00)

`--dbDir` makes corDB survive a restart: every write appends its effect to a log - memory-mapped, so
the record is in the kernel's page cache before the response - synced every 100 ms (`--dbSync
interval`, the default), with snapshots as the log grows
([corDB's persistence](https://github.com/SEAMWARE/corDB/blob/main/doc/persistence.md)).
`test/perf/perfRun.sh corDB`, PGO release, one tenant, the log on an NVMe disk (ext4), AMD Ryzen 9
8940HX (32 threads, nothing pinned), 2026-10-05 - requests/s, against the same broker without
`--dbDir`:

| scenario | in RAM | `--dbDir` | change | `--dbSync request` | change |
|---|---:|---:|---:|---:|---:|
| query, `limit=20`, c50 | 140 093 | 141 287 | +1 % | 127 746 | −9 % |
| `GET /entities/{id}`, c50 | 651 255 | 661 057 | +2 % | 581 824 | −11 % |
| `PATCH`, c50 | 142 304 | 111 438 | −22 % | 24 100 | −83 % |
| `PATCH`, c1 | 51 094 | 48 131 | −6 % | 1 809 | −96 % |
| merge, c50 | 84 944 | 73 824 | −13 % | 24 058 | −72 % |
| `DELETE`, c50 | 170 449 | 160 439 | −6 % | 23 407 | −86 % |
| batch update (20), c50 | 37 858 | 27 041 | −29 % | 13 826 | −63 % |
| batch delete (20), c50 | 56 348 | 50 834 | −10 % | 21 677 | −62 % |
| create, c50 | 124 399 | 84 444 | −32 % | 18 870 | −85 % |
| create, c1 | 37 839 | 33 175 | −12 % | 1 739 | −95 % |
| batch create (20), c50 | 69 536 | 21 444 | −69 % | 8 654 | −88 % |

- **Reads cost nothing.** A query or a retrieve never touches the log.
- **A write that changes the store costs 6-29 %** - the encoding of its record and its copy into the
  mapped log. A write that updates attributes logs those attributes, not the entity (corDB's design,
  § 3): the more attributes an entity has beside the one a PATCH touches, the more that saves.
- **Creates cost more because the store grows**, and a growing store needs snapshots: batch create
  loses 69 % of its throughput, its p99 22.2 ms against 1.93 ms in RAM - a snapshot takes the write lock
  for a few milliseconds at a time.
- **`--dbSync request` is the disk's speed**: a write answers when its record is synced, so one
  connection does ~1 800 writes/s and fifty share each sync (group commit) for ~19 000-24 000.
- **Recovery is not a pause worth planning for**: 100 000 entities (38 MB of log or of snapshot) are
  back in **0.19 s** after `kill -9` (the log replayed) or a clean stop (the snapshot loaded) -
  measured 2026-10-04, the format unchanged since.

## 2026-10-05 - an attribute update logs the attributes, not the entity

corDB logged a whole entity for every write, a PATCH of one attribute too. Now a write that updates
attributes logs those attributes (corDB's `ATTRS_PUT`): the members it named, each whole after the
write. On the perf fixture's entity (five attributes, ~550 bytes), PGO release, against the run before
it (corDB with the mapped log, same build otherwise):

| | before | after | |
|---|---:|---:|---:|
| `PATCH`, corDB on disk, 8 cores | 127 762 | 144 465 | +13.1 % |
| `PATCH`, corDB on disk + history, 8 cores | 107 860 | 119 962 | +11.2 % |
| `PATCH`, `--dbDir`, nothing pinned | 99 571 | 111 438 | +11.9 % |
| merge, corDB on disk / + history, 8 cores | 96 317 / 77 202 | 103 106 / 82 574 | +7.0 % |
| `PATCH`, `--dbSync request` | 22 558 | 24 100-25 301 | +7-12 % |
| *in RAM, which the change does not touch* | | | −2.8 % to +1.8 % |

An entity with a 2 KB attribute: twenty PATCHes of a small one grew the log by 47 194 bytes before,
under 20 KB now (`cordb_persist_attr_updates`). The bigger the entity, the bigger the saving.

The first `--dbSync request` run had `DELETE` at 10 582 (24 621 before): the run again, same build,
23 407. A write there waits for its sync, and that run's syncs stalled - the page has the second run.

## 2026-10-05 - every corDB-on-disk number measured again: the log memory-mapped, the databases on the host network

**Why.** corDB on disk buffered a write's log record in the process for up to 100 ms: a broker that
died (`kill -9`, a crash, an OOM kill) lost writes it had acknowledged, and the numbers published for
corDB on disk were the numbers of a store that could. The log is memory-mapped now (corDB): the record
is in the kernel before the response; only a machine that dies loses anything - the last 100 ms, as
MongoDB's default. Every number of corDB on disk was measured again; so was every number with a
database in a container: MongoDB and PostgreSQL ran behind a port mapping (`docker run -p`), a
userland proxy on every database round trip. Now `--network host`.

What changed between the tables below and today's:

- **The mapped log** costs a write 0-8.5 % (batch update the most), a read nothing - corDB's
  [persistence history](https://github.com/SEAMWARE/corDB/blob/main/doc/history/persistence.md) has
  the table, and the page preparation (`MADV_POPULATE_*`) tried and dropped.
- **The host network** gave Orion-LD + MongoDB + PostgreSQL 4-7 % on writes (create 4 656 → 4 991,
  PATCH 5 100 → 5 374, `DELETE` 8 813 → 9 457); its reads within the noise.
- **The worker pool** (as many workers as cores for a store that never waits, 2026-10-04) and **the id
  table growing in steps** (corDB) took the p99 of most rows down by a factor: a retrieve 4.3 ms →
  0.2 ms, ramDB's batch create 239 ms → 3.3 ms.
- **The PGO training** runs corDB on disk with its history too (`pgoTrain.sh`): +1-5 % on those rows
  against the build trained on corDB in RAM alone (create 81 754 → 84 559, PATCH 103 004 → 107 860).
- **Still open: batch delete** is 11 % (corDB) and 20 % (ramDB, no log at all) below the tables
  below - the pool or the id table, not the log; to be found.
- Orion-LD + TRoE crashed on a stop four times in one run (two SIGSEGV, two SIGABRT) - between
  scenarios, every number was in.

The slide for the FIWARE TSC (2026-10-05) carries the first run with the mapped log, before the PGO
training was extended - 2-5 % below this page's corDB columns.

#### Per core, before (libmicrohttpd, 2026-09-30/10-01, MongoDB 4.4 behind a port mapping)

| Response | req/s per core | entities/s per core |
|---|---:|---:|
| 1 entity | 74 915 | 74 915 |
| 20 entities | 9 822 | 196 440 |
| 100 entities | 2 082 | 208 200 |

libmicrohttpd + `mongoc`, `limit=20`: 5 858 req/s, 117 160 entities/s.

#### The eight-core tables before (2026-10-04: the log buffered in the process, the databases behind a port mapping)

**Requests/s:**

| | coraine + MongoDB | coraine + MongoDB + TimescaleDB | coraine, corDB on disk | coraine, corDB on disk + history | coraine, ramDB | Orion-LD + MongoDB | Orion-LD + MongoDB + PostgreSQL |
|---|---:|---:|---:|---:|---:|---:|---:|
| create | 44 252 | 4 128 | 95 870 | 83 967 | 178 969 | 6 353 | 4 656 |
| create, 1 connection | 7 255 | 645 | 28 116 | 22 913 | 31 305 | 3 449 | 897 |
| batch create (20) | 12 131 | 311 | 17 059 | 7 379 | 47 090 | 3 507 | 1 522 |
| merge | 26 506 | 6 614 | 97 777 | 79 135 | 124 705 | 6 267 | 4 989 |
| `PATCH` | 16 080 | 7 240 | 133 862 | 112 056 | 173 299 | 6 407 | 5 100 |
| `PATCH`, 1 connection | 2 874 | 816 | 37 611 | 35 787 | 39 932 | 3 008 | 848 |
| batch update (20) | 2 197 | 605 | 29 076 | 20 188 | 36 650 | 2 330 | 2 000 |
| `DELETE` | 28 821 | 8 942 | 201 909 | 196 254 | 214 114 | 18 119 | 8 813 |
| batch delete (20) | 6 963 | 1 659 | 74 373 | 54 247 | 83 702 | 3 105 | 2 400 |
| `GET /entities/{id}` | 54 394 | 55 293 | 432 549 | 434 841 | 428 273 | 31 622 | 31 460 |
| query, `limit=20` | 21 304 | 21 982 | 77 273 | 77 390 | 78 052 | 5 405 | 5 403 |

**Latency, p50 / p95 / p99, ms:**

| | coraine + MongoDB | coraine + MongoDB + TimescaleDB | coraine, corDB on disk | coraine, corDB on disk + history | coraine, ramDB | Orion-LD + MongoDB | Orion-LD + MongoDB + PostgreSQL |
|---|---:|---:|---:|---:|---:|---:|---:|
| create | 1.03 / 1.76 / 3.15 | 11.1 / 22.0 / 48.5 | 0.41 / 9.78 / 21.8 | 0.44 / 16.4 / 35.6 | 0.24 / 0.81 / 5.63 | 6.95 / 14.7 / 22.8 | 10.2 / 18.0 / 24.4 |
| create, 1 connection | 0.13 / 0.16 / 0.20 | 1.52 / 1.67 / 33.0 | 0.03 / 0.04 / 0.99 | 0.04 / 0.05 / 1.34 | 0.03 / 0.03 / 0.04 | 0.28 / 0.39 / 0.42 | 1.06 / 1.35 / 1.42 |
| batch create (20) | 3.55 / 9.16 / 28.0 | 141 / 300 / 410 | 2.14 / 12.1 / 20.3 | 4.50 / 30.3 / 62.3 | 0.79 / 10.8 / 239 | 12.7 / 28.8 / 40.1 | 30.9 / 53.4 / 68.3 |
| merge | 1.75 / 2.52 / 3.13 | 6.24 / 15.4 / 19.9 | 0.46 / 1.45 / 2.05 | 0.61 / 1.56 / 2.25 | 0.38 / 1.04 / 1.46 | 7.13 / 16.2 / 27.9 | 9.51 / 16.3 / 21.7 |
| `PATCH` | 2.92 / 3.99 / 4.65 | 5.78 / 12.9 / 16.3 | 0.37 / 1.07 / 1.55 | 0.42 / 1.30 / 1.89 | 0.26 / 0.81 / 1.12 | 6.82 / 16.5 / 31.8 | 9.30 / 16.0 / 21.3 |
| `PATCH`, 1 connection | 0.34 / 0.39 / 0.47 | 1.29 / 1.47 / 1.61 | 0.02 / 0.03 / 0.03 | 0.03 / 0.03 / 0.03 | 0.02 / 0.02 / 0.03 | 0.33 / 0.35 / 0.40 | 1.17 / 1.23 / 1.37 |
| batch update (20) | 21.5 / 28.1 / 31.1 | 73.8 / 153 / 193 | 1.62 / 3.73 / 5.39 | 2.26 / 4.62 / 6.45 | 1.34 / 2.77 / 3.78 | 19.4 / 37.9 / 50.2 | 22.3 / 46.2 / 59.0 |
| `DELETE` | 1.59 / 2.33 / 3.39 | 4.06 / 9.92 / 12.9 | 0.16 / 0.57 / 0.81 | 0.17 / 0.62 / 0.92 | 0.14 / 0.54 / 0.77 | 2.54 / 4.64 / 6.56 | 5.33 / 9.58 / 13.2 |
| batch delete (20) | 6.12 / 13.2 / 16.3 | 19.5 / 63.7 / 120 | 0.54 / 1.38 / 2.15 | 0.82 / 1.56 / 2.05 | 0.41 / 1.30 / 1.96 | 14.1 / 30.0 / 38.3 | 18.0 / 39.0 / 50.7 |
| `GET /entities/{id}` | 0.84 / 1.41 / 1.98 | 0.83 / 1.40 / 1.98 | 0.07 / 2.49 / 4.49 | 0.06 / 2.25 / 4.34 | 0.07 / 2.12 / 4.22 | 1.45 / 3.01 / 4.62 | 1.46 / 2.97 / 4.53 |
| query, `limit=20` | 2.15 / 3.57 / 5.08 | 2.08 / 3.44 / 4.87 | 0.32 / 6.21 / 9.02 | 0.30 / 6.22 / 9.19 | 0.32 / 5.66 / 6.50 | 8.35 / 15.5 / 20.7 | 8.40 / 15.8 / 21.9 |

Eight physical cores for the whole deployment - the broker, and the databases it needs (mongod 8.2 and
PostgreSQL 16 + TimescaleDB as containers pinned to the same cores); the load generator on the other
eight, standing in for clients. 50 connections unless said, median of 3 × 5 s, `test/perf/perfRun.sh`,
AMD Ryzen 9 8940HX, 2026-10-04. coraine a PGO release, corDB on disk with `--dbDir`; Orion-LD
1.15.0-next, its release build (`-O3`), `-mongocOnly`.

- **History is where it shows.** In a database server it costs a write 55-97 % (coraine + MongoDB +
  TimescaleDB: create 44 252 → 4 128); in corDB 3-57 %, and coraine with corDB and its history
  outruns MongoDB without any on every write but batch create - and MongoDB + TimescaleDB 12-44×.
- **Persistence costs little:** ramDB, no disk at all, is 1.0-1.9× corDB on disk; 2.8× on batch
  create, which grows the store fastest and so snapshots most.
- **Reads** do not touch history in any configuration. corDB answers a retrieve 8× MongoDB's rate; its
  p95/p99 there are higher (2-9 ms) because it serves 430 000 requests/s on the same 50 connections.
- **The tails of a write** follow the throughput: p99 of a batch update 6 ms (corDB + history) against
  193 ms (MongoDB + TimescaleDB).

#### What persistence costs, before (2026-10-04: the log buffered in the process)

`--dbDir` makes corDB survive a restart: every write appends its effect to a log,
synced every 100 ms (`--dbSync interval`, the default), with snapshots as the log
grows ([corDB's persistence](https://github.com/SEAMWARE/corDB/blob/main/doc/persistence.md)).
`test/perf/perfRun.sh corDB`, PGO release, one tenant, the log on an NVMe disk
(ext4), AMD Ryzen 9 8940HX (32 threads, nothing pinned), 2026-10-04 - requests/s,
against the same broker without `--dbDir`:

| scenario | in RAM | `--dbDir` | change | `--dbSync request` | change |
|---|---:|---:|---:|---:|---:|
| query, `limit=20`, c50 | 134 111 | 140 810 | +5 % | 139 692 | +4 % |
| `GET /entities/{id}`, c50 | 640 083 | 665 814 | +4 % | 638 030 | 0 % |
| `PATCH`, c50 | 140 886 | 103 343 | −27 % | 16 525 | −88 % |
| `PATCH`, c1 | 51 414 | 48 155 | −6 % | 1 311 | −97 % |
| merge, c50 | 87 189 | 71 410 | −18 % | 16 072 | −82 % |
| `DELETE`, c50 | 173 285 | 160 999 | −7 % | 15 033 | −91 % |
| batch update (20), c50 | 38 330 | 31 998 | −17 % | 16 665 | −57 % |
| batch delete (20), c50 | 59 495 | 53 829 | −10 % | 19 548 | −67 % |
| create, c50 | 124 364 | 77 424 | −38 % | 15 083 | −88 % |
| create, c1 | 38 333 | 33 160 | −13 % | 1 205 | −97 % |
| batch create (20), c50 | 67 629 | 16 497 | −76 % | 10 260 | −85 % |

- **Reads cost nothing.** A query or a retrieve never touches the log.
- **A write that changes the store costs 6-27 %** - the encoding of its record
  (the whole entity today; a PATCH that logs only the attributes it touched is
  the next step - corDB's design, § 3).
- **Creates cost more because the store grows**, and a growing store needs
  snapshots: batch create, which grows it by a million entities in ten seconds,
  loses 76 % of its throughput. Its p99 drops by 94 % (257 ms to 16 ms): a
  snapshot takes the write lock for at most a few milliseconds at a time.
- **`--dbSync request` is the disk's speed**: a write answers when its record is
  synced, so one connection does ~1 200-1 300 writes/s and fifty share each sync
  (group commit) for ~15 000-16 500.
- **Recovery is not a pause worth planning for**: 100 000 entities (38 MB of log
  or of snapshot) are back in **0.19 s** after `kill -9` (the log replayed) or a
  clean stop (the snapshot loaded). A clean stop of that store, its last sync and
  its snapshot included, takes 0.19 s.

## 2026-10-04 - eight cores: coraine and Orion-LD, MongoDB, TimescaleDB, corDB, ramDB

"One machine, eight cores" in [the performance page](../performance.md) measured again: corDB on disk
now, its history in the store, ramDB, Orion-LD beside coraine, the databases as containers pinned to
the same eight cores, p50 and p95 beside p99 (`perfRun.sh` gained them: a `done()` hook in every wrk
run). What the run found on the way:

- **ramDB ran without the fast paths.** Inline dispatch and coroutines were switched on for the plugin
  NAMED corDB; ramDB - corDB in RAM only - ran every request on a worker: its retrieve 2.4× below
  corDB's. Asked of what the store is now (`neverWaits`): 178 000 → 428 000 retrieves/s.
- **The first ramDB column was a debug ramDB.so**: the local corDB `main` was not pulled after corDB#4,
  so `make pgo` built no ramDB and an old debug build stayed installed. Measured again.
- **Orion-LD**: the installed binary was a debug build; its release build (`make release`, `-O3`) was
  measured, run from its build directory. With MongoDB 8 it needs `-mongocOnly` - the legacy driver's
  OP_QUERY is gone from MongoDB 8. One Orion-LD + TRoE broker aborted on a stop between two scenarios;
  every number was in.

#### The table before (corDB in RAM, no Orion-LD, p99 only)

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

> ⚠️ The `corDB` rows above are corDB **in RAM** (no `--dbDir`). What
> persistence costs is the next section. It costs no libraries — everything it
> needs is in libc.


## 2026-10-04 - corDB measured on the wrong build

From corDB's move to its own repository (2026-10-03 12:40) a local `make pgo` ended with `make di` on
every library it had built with the profile. corDB is a plugin, not an archive linked into the PGO
broker, so the **debug** corDB.so (no `-O`, traces compiled in) stayed installed beside the
profile-guided broker; `make install_pgo` did not install corDB at all. The Docker image and the
nightly (`PGO_RESTORE_DEBUG=0`) were not affected. `make install_pgo` now installs corDB's PGO release
plugins.

"corDB on disk" in [the performance page](../performance.md) was measured again with it (32 threads,
nothing pinned). Before → after, `--dbDir` against in RAM: PATCH c50 −24 → −27 %, merge −14 → −18 %,
DELETE −6 → −7 %, batch update −12 → −17 %, batch delete −11 → −10 %, create c50 −28 → −38 %, create c1
−11 → −13 %, batch create −72 → −76 %; recovery of 100 000 entities 0.21 → 0.19 s. The in-RAM baseline
got faster (PATCH 132 → 141k, create 110 → 124k, batch create 48 → 68k req/s); the log's cost did not,
so it weighs more.

One of the three runs was lost: its broker was killed (SIGKILL) during the 200-connection query, with
no OOM kill, no systemd-oomd entry and nothing in the broker that sends one; the same run again was
clean. Noted, not explained.

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
