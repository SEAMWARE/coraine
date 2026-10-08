# Migrating a database to coraine

A migration moves what another broker holds - entities, subscriptions, context source registrations,
and the temporal history - into coraine's stores, with the identities and the system timestamps it
had there.

It has two halves that meet in one intermediate representation, the **migration stream**:

```
  source database            READER                     STREAM                 WRITER                       target stores
 ─────────────────     ──────────────────────     ───────────────────     ─────────────────────     ─────────────────────────
 Orion-LD  MongoDB ──▶ tools/migrate/           ──▶                   ──▶ coraine-import       ──▶ current state: mongoc | corDB
 Orion-LD  TRoE    ──▶   orionldExport.py           one JSON record       (the broker's code    history:       timescale | corDB
 (later) NGSIv2 ...──▶ (one reader per source)      per line, expanded     and store plugins)
                                                    NGSI-LD
```

A reader knows its source and nothing about coraine's storage. The writer, `coraine-import`, is a
program of its own, linked against the broker's libraries and loading the broker's store plugins: it
knows nothing about any source, and what it stores is always what the current broker stores. The
broker itself carries no import code.

```console
tools/migrate/orionldExport.py --mongo mongodb://localhost:27017 --db orion \
                               --troe 'host=localhost user=postgres password=...' --out stream.ndjson
coraine-import -db corDB --dbDir /var/lib/coraine --troe corDB --file stream.ndjson
```

`coraine-import` takes the broker's store options, by the broker's names (`--database`/`-db`,
`--troe`, and each plugin's own: `--dbDir`, `--dbName`, `--dbHost`, `--troeName`, ...) and `--file`
(`-` = stdin). It writes the stream, prints a report on stderr (its log is stdout) and exits: 0 when
every record was imported, 1 when any was refused. Then the broker is started the usual way on the
same stores. The importer and a broker never share a store: the import runs before the broker starts.

`coraine-import` is built with `COR_FEATURE_MIGRATE` (on by default); off, it is not built.

## Step by step

1. **Stop the writes to the source.** What is written after the export is not in the stream. A
   deployment that cannot stop can export once, import, and export + import again what changed -
   but a record already imported is refused the second time (see 5), so a second pass is for new
   entities, not for changed ones.
2. **Export.** From a checkout of this repository, with the source's MongoDB (and its TRoE
   PostgreSQL, for the history) reachable:

   ```console
   pip install pymongo psycopg            # psycopg only for the history
   tools/migrate/orionldExport.py --mongo mongodb://orion-mongo:27017 --db orion \
                                  --troe 'host=orion-pg user=postgres password=...' \
                                  --out stream.ndjson
   ```

   | Option | Meaning | Default |
   |---|---|---|
   | `--mongo` | MongoDB URI of the current state | `mongodb://localhost:27017` |
   | `--db` | the database-name prefix the deployment ran with (`-db`) | `orion` |
   | `--troe` | libpq connection string of the TRoE server, without `dbname`; `none`: no history | none |
   | `--troeDb` | the TRoE database-name prefix | the `--db` prefix |
   | `--tenants` | comma-separated tenants (`default` or `''` for the default tenant) | all |
   | `--noCurrent` | leave the current state out (history only) | |
   | `--out` | the stream file | stdout |

   What the exporter leaves out says so on stderr ([Reading an Orion-LD database](#reading-an-orion-ld-database)).
3. **Import**, into the stores the broker will run on, with the broker NOT running on them:

   ```console
   coraine-import --database mongoc --dbHost mongo --troe timescale --troeHost pg --file stream.ndjson
   coraine-import --database corDB  --dbDir /var/lib/coraine --troe corDB      --file stream.ndjson
   ```

   The report, on stderr:

   ```
   import of stream.ndjson:
     entities                        3 imported,      0 refused
     subscriptions                   3 imported,      0 refused
     registrations                   1 imported,      0 refused
     temporal entity events          4 imported,      0 refused
     temporal instances              8 imported,      0 refused
     created rows (history)          1 written,       0 failed
   ```

   Exit code 0: everything was imported. 1: something was refused - each refused record is named
   above the report by its line in the stream and the reason:

   ```
   stream.ndjson:3: entity: entity type 'Room' is not expanded - the stream must hold expanded NGSI-LD (full IRIs, core terms short)
   ```
4. **Start the broker** on the same stores, the usual way, and compare: the number of entities per
   type (`GET /ngsi-ld/v1/types?details=true`), the subscriptions, the registrations, and a few
   entities' temporal evolution against the source.
5. **A refused record** is not written, and the others are: the import does not stop at the first
   one. Fix those lines of the stream (or the reader) and import a stream of just them. A record
   that is already in the target store is refused too - an import never overwrites - so importing
   the whole stream again refuses everything that went in the first time.

### In a container

`coraine-import` is in the image beside `coraine`:

```console
docker run --rm -v $PWD:/migrate --network <the stores' network> --entrypoint coraine-import \
       quay.io/seamware/coraine:<tag> --database mongoc --dbHost mongo --file /migrate/stream.ndjson
```

On Kubernetes, a one-off **Job** before the broker starts, on the broker's stores - for a persistent
corDB the same volume the broker's StatefulSet mounts (a ReadWriteOnce volume: run the Job with the
StatefulSet at zero replicas, then scale it up). Not an initContainer: it would run again at every
restart of the pod, and a second import refuses everything and exits 1.

## The stream

One JSON object per line ([NDJSON](https://github.com/ndjson/ndjson-spec)), version 1:

```json
{"kind":"header","format":"coraine-migration","version":1,"source":"orion-ld"}
{"kind":"registration","tenant":"","data":{ ...a ContextSourceRegistration... }}
{"kind":"subscription","tenant":"","data":{ ...a Subscription... }}
{"kind":"entity","tenant":"t1","data":{ ...an Entity... }}
{"kind":"temporalEntity","tenant":"","data":{"id":"urn:E1","type":"https://ex.org/T","op":"created","at":"2024-01-02T03:04:05.123Z"}}
{"kind":"temporalInstance","tenant":"","data":{"id":"urn:E1","type":"https://ex.org/T","attr":"https://ex.org/speed","op":"modified","instance":{ ... }}}
```

| member | |
|---|---|
| `kind` | `header` (optional, first line), `entity`, `subscription`, `registration`, `temporalEntity`, `temporalInstance` |
| `tenant` | the tenant; `""` or absent: the default tenant |
| `data` | the record, in NGSI-LD's own representation - below |

The data is **NGSI-LD as the API represents it, with every name expanded** (full IRIs; a core term
such as `location` may stay short) - nothing of any broker's storage - whatever the source. That makes
the stream readable without coraine and close to what an ETSI neutral export format will have to carry.

- **entity** - an Entity, normalized, with its system attributes: `createdAt` / `modifiedAt` on the
  entity, on every attribute instance and on every Sub-Attribute. A multi-instance attribute is an
  array, each instance with its `datasetId`.
- **subscription** - a Subscription, with `id`, `createdAt`, `modifiedAt`, and in `notification`
  the counters a GET shows: `timesSent`, `timesFailed`, `lastNotification`, `lastSuccess`,
  `lastFailure`. A `q` names attributes by IRI with the dots of the IRI written `%2E` (in a q a dot
  separates an attribute from its sub-attribute).
- **registration** - a ContextSourceRegistration, with `id`, `createdAt`, `modifiedAt`.
- **temporalEntity** - an entity-level history event: `op` `created` | `replaced` | `deleted`, at `at`.
- **temporalInstance** - one attribute instance of the history: the entity's `id` and `type`, the
  attribute `attr`, `op` `created` | `modified` | `replaced` | `deleted`, and the `instance` with its
  `instanceId`, `createdAt`, `modifiedAt` (a deletion: `deletedAt`) and `datasetId`.

The writer imports the records in the order they come; the history is written in batches of 500
events, one transaction each. A reader therefore writes the registrations first (an exclusive or
redirect registration is refused over local entities that hold what it claims), and the
`temporalEntity` records before the `temporalInstance` records of the same entity.

## Names: expanded, always

The importer expands nothing and takes no @context. Every name that NGSI-LD expands - an entity type,
an attribute name, a Sub-Attribute name, a VocabProperty's vocab, the names in a subscription's
`entities`, `watchedAttributes` and `notification.attributes`, in a registration's `information`, a
history record's `type` and `attr` - must be an absolute IRI (`http://`, `https://`, `urn:`) or an
NGSI-LD core term. Anything else - `Room`, `temperature`, a compact IRI such as `ex:humidity` - means
the stream is not expanded: **the record is refused**, and the report names the term. The member names
inside a compound value are the application's own JSON and are not checked.

An Orion-LD database stores names expanded (core terms short), so the Orion-LD reader writes them as
they are. A reader for a source that does not expand names expands them itself (below).

## How the writer writes

`coraine-import` loads the DB and TRoE plugins, brings the stores and the caches the create routines
consult up as the broker does, and writes every record through the code a live write goes through:

| record | written by |
|---|---|
| entity | the conversion a create runs - JSON-LD expansion, `ldNormalizeInput`, `ldCheckEntity`, `ldApiEntityToDbModel` - then `db.entityCreate`. The system timestamps are taken out before the conversion and put back on the DB model after it, on the entity, every instance and every Sub-Attribute; the driver then stores them its own way (once per entity where an attribute's equal the entity's). |
| subscription | the service routine of `POST /subscriptions`, `postSubscriptions`, in-process - its validation, the stored `q`, `status`, `jsonldContext` - with the request clock at the source's `createdAt`; then `db.subscriptionUpdate` for `modifiedAt` and `db.subscriptionStatsFlush` for the counters |
| registration | `postCsourceRegistration` the same way; `modifiedAt` set on the stored registration as a PATCH sets it (`db.registrationUpdate`) |
| temporalEntity, temporalInstance | the instance converted like an entity's, as an event of the TRoE plugin carrying the source's `instanceId` and `createdAt` (`TroeEvent.instanceId`, `createdAtNs`). A plugin with `TroeDriver.historyImport` takes them there (`--troe corDB`), any other through its live event entry points (`TroeDriver.eventList` - timescale), where a live write leaves both unset and the plugin generates them |

So no storage format is written by anything but the plugin that owns it - mongoc's documents, corDB's
log, snapshots and history log, timescale's tables - and a change to one of them needs no change here.
Nothing a live write does besides storing happens: no notification, no forwarding to a registration,
no history for the current state - corDB, which records history at its own write sites, is told not
to (`corNgsild.troeSkip`, as a Snapshot's capture tells it). The history is what the stream's
`temporal*` records say:

- **corDB** (`TroeDriver.historyImport`): the entity events at their time, every instance with the
  source's instanceId, createdAt, modifiedAt and observedAt, into corDB's history index and history log
  (`hist-N.cor`), as the temporal API's own writes are - not through them (they give an instance a new
  id and the request's time). A deletion becomes the tombstone a live deletion writes: the attribute's
  type (the record's, else the type of the attribute's last instance), `urn:ngsi-ld:null`, `deletedAt`.
- **timescale**: one transaction per batch; an imported instance keeps its instanceId and createdAt.

**An entity the stream gives no history.** With a TRoE store, an entity of the stream's current state
that no `temporal*` record names gets the history a create writes: its `created` event at its
createdAt, and an instance of each attribute as it is now, at the instance's own createdAt and
modifiedAt (the value it holds is the one it has had since its modifiedAt), its instanceId generated
by the store. The report counts them (`created rows`). An entity the stream has history for - even a
history record that was refused - gets what the stream holds and nothing more.

**Why not the API.** A create through the API cannot keep `createdAt`, `modifiedAt`, a subscription's
counters or an instance's `instanceId` - the broker sets them, as it must. **Why not the files.**
Writing mongoc documents or corDB log records from outside would be a second copy of each format, to
be kept in step with the broker forever. **Why not an import route on a serving broker.** A route that
lets a client set system timestamps is a hole in what a broker guarantees about them; a migration is a
run of its own, on stores nothing else is using yet.

## What is kept

| | kept | notes |
|---|---|---|
| entity id, type(s), scope, expiresAt | yes | an entity past its expiresAt is imported, and expires as any other |
| attribute values, every attribute type, datasetId instances, Sub-Attributes, observedAt, unitCode | yes | |
| createdAt / modifiedAt - entity, instance, Sub-Attribute | yes | a level without its own gets the level above's |
| tenants | yes | the source's tenant names, lowercased as coraine names tenants |
| subscription id, every member, createdAt, modifiedAt | yes | `status` is recomputed (it is computed, never stored); `isActive: false` is kept |
| subscription counters - timesSent, timesFailed, lastNotification, lastSuccess, lastFailure | yes | |
| registration id, every member, createdAt, modifiedAt | yes | |
| history - instanceId, createdAt, modifiedAt, observedAt, deletions, deleted entities | yes | timescale and corDB |
| history of an entity the stream has none for | its created row | see above |
| registration counters | no | the source keeps none in its database |
| a subscription's subordinate subscriptions (distributed) | no | recreated by the broker from the registrations |
| hosted / cached @contexts | no | not yet |

An import goes into **empty** stores: a record whose id is already there is refused, never overwritten,
and a history batch with an instanceId already there is refused as a whole (corDB looks for the first
instance of each attribute of the batch - the one a re-import repeats).

## Reading an Orion-LD database

`tools/migrate/orionldExport.py` (Python 3; `pymongo` for the current state, `psycopg` 3 or `psycopg2`
for the history) reads an Orion-LD deployment's databases and writes the stream. It reads, per tenant:

| Orion-LD | read from | becomes |
|---|---|---|
| current state | MongoDB `<db>` (default tenant) and `<db>-<tenant>`; collections `entities`, `csubs`, `registrations` | entity, subscription, registration records |
| history (TRoE) | PostgreSQL `<db>` and `<db>_<tenant>`; tables `entities`, `attributes`, `subAttributes` | temporalEntity, temporalInstance records |

The conversions, in short: a stored key's `=` is a `.` of the expanded name; epoch-second timestamps
become ISO 8601; an attribute's `value` becomes the member its type names (`object`, `languageMap`,
`vocab`, `json`); `md` entries become `observedAt`, `unitCode` and Sub-Attributes; `@datasets`
instances join the default instance in an array; `csubs` become Subscriptions (`ldQ` → `q`, the
stored geo-expression → `geoQ`, `count` → `timesSent`, `headers` → `receiverInfo`, `status`
`paused`/`inactive` → `isActive: false`); `contextRegistration` becomes `information` + `endpoint`;
history rows become instances by their `valueType`, sub-attribute rows join their attribute's instance,
`opMode` `Create`/`Append` → `created`, `Update` → `modified`, `Replace` → `replaced`, `Delete` →
`deleted`.

Not exported, each said on stderr: NGSIv2 subscriptions in `csubs` (an ObjectId `_id`), the `casubs`
collection, and history rows of an entity whose type neither the history nor the current state knows.
A history value is what the history table kept: an integral number comes back as an integer, and
attribute types that table does not record come back as Properties.

## Readers to come

A reader is a program that writes the stream; the writer does not change for a new source.

- **Orion (NGSIv2).** A script: the current state only - entities (attributes and their metadata -
  metadata become Sub-Attributes, `dateCreated` / `dateModified` the system timestamps),
  subscriptions, registrations. NGSIv2 expands nothing, so the reader takes the user's @context (a file
  or a URL) and expands every name with it, before the stream is written: **a term the context does
  not define is an error**. Whether a term may instead be expanded through the context's `@vocab` is
  the end user's choice - an option of the reader, off by default. We do not recommend it: `@vocab`
  gives any word an IRI, typos included, and the IRI it gives is one nobody chose.
- **History kept outside the broker** - CrateDB or PostgreSQL tables written by a history service of
  an NGSIv2 deployment. A reader per layout, writing `temporalEntity` / `temporalInstance` records;
  where the source has no instance ids, the reader omits `instanceId` and the TRoE plugin generates them.
- **An ETSI neutral export format.** Once ETSI TC DATA defines one, a reader from it (and, for
  exports, a writer to it) - the stream already holds the same content: expanded NGSI-LD with system
  attributes.

## Not yet

- @contexts (hosted, cached) of the source.

## Testing

- `test/funcTests/cases/migrate_import.test` imports `test/funcTests/fixtures/migrate/orionld-stream.ndjson`
  into the suite's store (mongoc or corDB) and timescale and reads everything back through the API - the
  created row of the entity without history included - then a stream that is not expanded and a second
  import.
- `test/funcTests/cases/migrate_import_cordb_troe.test` imports the same stream into corDB with
  `--troe corDB`: the history as the source had it, nothing on top, the created row, the same after a
  restart (the history log), a second import refused.
- `tools/migrate/test/orionldE2E.sh` is the whole chain: it fabricates an Orion-LD deployment (MongoDB
  and PostgreSQL databases named `migtest*`), reads it with `orionldExport.py`, checks the stream is the
  functest's fixture, imports it with `coraine-import` into corDB + timescale and into mongoc + timescale, and checks both
  read back alike and as `orionldE2E.expected` says. It needs a python with `pymongo` and `psycopg`
  (`PYTHON=`), and drops only the `migtest*` databases it made.
