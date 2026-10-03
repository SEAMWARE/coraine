# corDB persistence - design

*Draft, 2026-10-02. The concrete form of `ToDo.md` § 15 steps 1-2 - the record format, and log +
snapshot + recovery. History (the selector, retention, the temporal index) builds on it later and is
only constrained here, not designed.*

## 1. What it has to do

corDB keeps every tenant's entities, subscriptions and registrations in RAM, one `CorNode` tree per
tenant (`corDbStore.h`). A restart loses all of it. After this step:

- a broker **stopped cleanly** (SIGTERM, SIGINT) loses nothing: everything in RAM is on disk before
  it exits (§ 5a)
- a broker that **dies** - `kill -9`, a crash, a power cut - comes back with what it had, less at
  most the last ~100 ms of writes (the default; MongoDB's journal makes the same promise) - nothing,
  with `--dbSync request`
- recovery time is bounded by a snapshot, not by the age of the broker
- **no new library**: `open`, `write`, `fdatasync`, `rename`, `ftruncate` - and the cor:// codec the
  broker already has

Not in this step: history retention, the selector, the temporal index, a standalone corDB server.
Each needs the log; none changes its format (§ 7).

## 2. The shape

```
                    write path (under the tenant's write lock)
  request ─► corDB mutates the tree ─► appends the EFFECT as a record ─► unlock ─► response
                                                  │
                                                  ▼
                               per-tenant log buffer ──► flusher thread: write + fdatasync
                                                                         every 100 ms
  recovery:  newest valid snapshot  ─►  replay the log after it  ─►  serve
```

**One directory per tenant** (`<--dbDir>/<tenant>/`, `_` for the default tenant): the lock, the store
and the files are all per tenant already, and a tenant drop becomes a directory delete.

```
  snap-000041.cor     the tree as of log sequence 41 (complete, or not there at all)
  log-000041.cor      every record since snapshot 41
  log-000042.cor      ... after the next snapshot
```

## 3. What a record is: the EFFECT, not the request

A record says what the write **did to the tree**, at attribute granularity - never the request that
did it. Replaying a request would mean replaying NGSI-LD semantics (merge, `observedAt` handling, the
`@context`, `noOverwrite` ...) with the code of the day of the replay, not of the day of the write; two
versions of the broker would rebuild two different stores from one log. An effect replays with no
semantics at all: "these attributes now are exactly this".

| op | body | written by |
|---|---|---|
| `ENTITY_PUT` | the whole entity, as stored | create, replace |
| `ATTRS_PUT` | entity id + the attributes it touched, each **whole, after the change** | update, merge, PATCH, attribute append - and each entity of a batch update/upsert/merge |
| `ATTRS_DELETE` | entity id + attribute names (+ datasetId) | attribute delete |
| `ENTITY_DELETE` | entity id | delete, batch delete |
| `SUB_PUT` / `SUB_DELETE` | the subscription / its id | subscription create, update, replace, delete |
| `REG_PUT` / `REG_DELETE` | the registration / its id | registration create, update, delete |
| `BATCH_BEGIN` / `BATCH_END` | count | a batch: replayed all or nothing |

`ATTRS_PUT` carries the attribute after the merge, not the merge's input, so a one-attribute PATCH of
a 20-attribute entity logs one attribute (~100 bytes), not the entity (~2 KB) - at 100 000 PATCH/s that
is the difference between 10 MB/s and 200 MB/s of log.

The 17 places that take `COR_DB_WRITE` are the places that append: each knows exactly what it
changed, and appends it before the lock goes. Lock order is log order.

## 4. The record format

```
  offset  size  field
  0       4     magic + version   "cr" 0x01 <op>
  4       4     body length (bytes)
  8       8     sequence (per tenant, monotonic - the snapshot's name is one)
  16      8     system time, ns   (= createdAt/modifiedAt/deletedAt of what it wrote: § 4.10a of cor-protocol.md)
  24      4     CRC-32C of bytes 0-23 + the body
  28      n     body: the cor binary tree (corTree corTreeBin, doc/cor-protocol.md § 4)
```

- **The body is the cor:// codec** - one serializer for the wire, the log and the snapshot
  (`ToDo.md` § 15: "specifying it once is the difference between one serializer and two that
  diverge"). Core terms are 1-byte ids, as on the wire.
- **Every record decodes on its own**: the codec's preloaded namespace table only, no per-connection
  string table (cor:// builds one as a connection goes; a log record must not depend on the records
  before it - for a torn tail now, for the temporal index's random access later).
- **The system time is in the header**, as an integer - KZ 2026-10-01: system timestamps are corDB's,
  stamped under the write lock, kept beside the node and not as tree members. History's other axis
  (`observedAt`) is in the attributes themselves.
- **CRC-32C** (SSE4.2 / ARMv8 CRC instructions; a table where neither) - a torn or rotten record is
  detected, never applied.

## 5. Writing: group commit on a timer

A write appends its record to the tenant's **log buffer** under the write lock (memcpy - no system
call under the lock). One **flusher thread** per broker, every `--dbSyncInterval` ms (default 100):
for each tenant with something buffered, swap the buffer out, `write()` it, `fdatasync()`.

- the response does not wait for the disk: what a power cut can lose is the last interval, as with
  MongoDB's default journal
- `--dbSync request`: the response waits for its record's `fdatasync` - group commit: every writer
  that arrived during one sync shares the next (the coroutines yield on it; a worker waits on a
  condition). Per-request durability, paid for by whoever asks
- `--dbSync none`: no fsync at all (tests, benchmarks, a RAM disk) - the log is still written
- a write error (disk full) is not silent: logged, a metric, and with `--dbSync request` the request
  fails 503 - a broker that acknowledges what it cannot keep is worse than one that refuses

## 5a. A clean stop loses nothing

On SIGTERM or SIGINT, in this order:

1. **no new requests** - the servers stop taking them; the requests in flight finish. A write appends
   its record under the tenant's write lock, so once the locks are free every acknowledged write is
   in a log buffer
2. **every tenant's log buffer written and `fdatasync`ed** - nothing that was only in RAM is left
3. **a snapshot per tenant** (§ 6) - the next start loads it and has no log to replay

Only then does the broker exit. A stop that takes too long is still a clean stop: step 2 does not
depend on step 3, so a broker killed during its snapshots has lost nothing either - it replays the
log on the next start. Only an end without a stop - `kill -9`, a crash, a power cut - can lose the
writes since the last sync (§ 5).

## 6. Snapshots and recovery

**Snapshot** (every `--dbSnapshotEvery` records or bytes of log, and at a clean shutdown): under the
tenant's **write lock** - the store as one cor tree, written to `snap-N.tmp`, `fdatasync`, `rename`
to `snap-N.cor`, `fsync` the directory; a new `log-N.cor` begins. At ~350 MiB per 100 000 entities
that holds the writers for well under a second, and it is obviously correct, which a copy-on-write
scheme is not. Older snapshots and logs are deleted once the new snapshot is durable (history, later,
keeps them: § 7).

**Recovery**, at start, per tenant directory:

1. the newest `snap-N.cor` that decodes in full (a `.tmp` is an interrupted snapshot: deleted)
2. `log-N.cor`, `log-N+1.cor` ... in order, record by record, until a record that is short or fails
   its CRC: **the torn tail** - the file is truncated there (`ftruncate`) and writing continues after it
3. the indexes (`corDbIndex`) are rebuilt from the tree, as at any load

Measured target: recovery at memory-bandwidth speed - decoding cor binary, no JSON parse.

## 7. What history adds later, and why nothing here stops it

- **History on** = keep logs and snapshots past the newest snapshot, by the retention policy
  (duration / bytes - § 15 "still open"). Compaction - deleting old segments - is where the TRoE
  boolean, the selector and the retention policy meet; in this step it just deletes.
- **The selector** (what history records) marks the record - a flag in the op byte - and compaction
  honours it. It does not change what durability writes.
- **"The entity as at T" by system time** = the snapshot at or before T, plus its log up to T: the
  header's system time makes that a prefix, and keeping a chain of snapshots makes it cheap.
- **The temporal index** - `(entity, attribute, time) -> record offset` - needs records that decode
  alone, which § 4 gives them.
- **cor:// replication / standalone corDB** (haaux): the records are already cor frames.

## 8. Options

| option | default | |
|---|---|---|
| `--dbDir <path>` | none: no persistence, as today | the tenants' directories |
| `--dbSync interval\|request\|none` | `interval` | § 5 |
| `--dbSyncInterval <ms>` | 100 | |
| `--dbSnapshotEvery <MiB of log>` | 64 | and at a clean stop |

No `--dbDir`, no change: a corDB without a directory is the in-RAM store of today, at today's speed.

## 9. Order of work, and how each step is proven

1. **The record writer and reader** in corDB, unit-tested: encode every op, decode it back; a torn
   tail and a flipped bit are found and stop the replay.
2. **The 17 write sites append**; the flusher; `--dbDir`, `--dbSync`.
3. **Recovery**: snapshot load + replay. Functests: write, `kill -9`, restart, read it all back -
   entities, attributes after PATCH/merge/delete, subscriptions, registrations, two tenants; a
   truncated log; an interrupted snapshot (`.tmp`).
4. **Snapshots** + log rollover; recovery from snapshot + log.
5. **Measure**: write throughput with `--dbSync interval` against no `--dbDir` (the bar: within a few
   per cent), `request` against it, recovery time for 100 000 entities. Documented in
   `doc/performance.md`, the losers too.

## 10. Open

- Snapshot trigger: log bytes only, or also a time?
- The CRC: CRC-32C (hardware on x86-64 and ARMv8) or xxHash3 (faster in software, no tables)?
- Hosted `@context`s and other per-tenant state that is not in the store tree today - persisted
  through the same log, or out of scope? (A list is the first job of step 2.)
