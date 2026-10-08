# Migrating a database to coraine

A migration moves what another broker holds - entities, subscriptions, context source registrations,
and the temporal history - into coraine's stores, with the identities and the system timestamps it
had there.

It has two halves that meet in one intermediate representation, the **migration stream**:

```
  source database            READER                     STREAM                 WRITER                       target stores
 ─────────────────     ──────────────────────     ───────────────────     ─────────────────────     ─────────────────────────
 Orion-LD  MongoDB ──▶ tools/migrate/           ──▶                   ──▶ coraine --importFile ──▶ current state: mongoc | corDB
 Orion-LD  TRoE    ──▶   orionldExport.py           one JSON record       (the broker's own code,   history:       timescale
 (later) NGSIv2 ...──▶ (one reader per source)      per line, expanded     then it exits)
                                                    NGSI-LD
```

A reader knows its source and nothing about coraine's storage. The writer is the broker itself, so
it knows nothing about any source, and what it stores is always what the current broker stores.

```console
tools/migrate/orionldExport.py --mongo mongodb://localhost:27017 --db orion \
                               --troe 'host=localhost user=postgres password=...' --out stream.ndjson
coraine -db corDB --dbDir /var/lib/coraine --troe timescale --troeName coraine --importFile stream.ndjson
```

`--importFile` takes the store options of a normal start (`-db`, `--dbDir`, `--dbName`, `--troe`,
`--troeName`, ...), writes the stream, prints a report and exits: 0 when every record was imported, 1
when any was refused. Then the broker is started the usual way on the same stores.

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
such as `location` may stay short) - nothing of any broker's storage. That makes the stream readable
without coraine and close to what an ETSI neutral export format will have to carry.

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

## Names that are not expanded: the @context

A source that does not expand names - NGSIv2 - is read as it is, and the import expands them with an
@context given by the user:

```console
coraine ... --importFile stream.ndjson --importContext migration-context.jsonld
```

`--importContext` is a URL or a file holding `{"@context": ...}`. Every name that has to be expanded -
an entity type, an attribute name, a Sub-Attribute name, a VocabProperty's vocab, the names in a
subscription's `entities`, `watchedAttributes` and `notification.attributes`, in a registration's
`information` - must be an IRI, a core term, a term the context defines, or a `prefix:suffix` whose
prefix it defines. **Anything else is an error**: the record is refused and the report names the term.
A name is never left to the default `@vocab` - that would invent an IRI nobody chose. The member names
inside a compound value are the application's own JSON and are not checked (the broker expands them
as it does those of any write).

Without `--importContext` the same check runs with no user context: every name must be an IRI or a
core term. That is how an Orion-LD stream is checked - Orion-LD stores names expanded.

## How the writer writes

`--importFile` runs after the broker has loaded its DB and TRoE plugins and before it serves anything,
and writes every record through the code a live write goes through:

| record | written by |
|---|---|
| entity | the conversion a create runs - JSON-LD expansion, `ldNormalizeInput`, `ldCheckEntity`, `ldApiEntityToDbModel` - then `db.entityCreate`. The system timestamps are taken out before the conversion and put back on the DB model after it, on the entity, every instance and every Sub-Attribute; the driver then stores them its own way (once per entity where an attribute's equal the entity's). |
| subscription | the service routine of `POST /subscriptions`, `postSubscriptions`, in-process - its validation, the stored `q`, `status`, `jsonldContext` - with the request clock at the source's `createdAt`; then `db.subscriptionUpdate` for `modifiedAt` and `db.subscriptionStatsFlush` for the counters |
| registration | `postCsourceRegistration` the same way; `modifiedAt` set on the stored registration as a PATCH sets it (`db.registrationUpdate`) |
| temporalEntity, temporalInstance | the TRoE plugin's event entry points (`TroeDriver.eventList`), the instance converted like an entity's. The event carries the source's `instanceId` and `createdAt` (`TroeEvent.instanceId`, `createdAtNs`); for a live write both are unset and the plugin generates them as before |

So no storage format is written by anything but the plugin that owns it - mongoc's documents, corDB's
log and snapshots, timescale's tables - and a change to one of them needs no change here. Nothing a
live write does besides storing happens: no notification, no forwarding to a registration (the import
runs local, `--distributed` or not), no history event from the broker for the current state - the
history is what the stream's `temporal*` records say. (`--troe corDB` is the exception: corDB records
history at its own write sites, so there the imported current state becomes history too - see below.)

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
| history - instanceId, createdAt, modifiedAt, observedAt, deletions, deleted entities | yes | |
| registration counters | no | the source keeps none in its database |
| a subscription's subordinate subscriptions (distributed) | no | recreated by the broker from the registrations |
| hosted / cached @contexts | no | not yet |

An import goes into **empty** stores: a record whose id is already there is refused, never overwritten
(and a history batch with an instanceId already there is refused as a whole).

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

- **Orion (NGSIv2).** The current state only: entities (attributes and their metadata - metadata
  become Sub-Attributes, `dateCreated` / `dateModified` the system timestamps), subscriptions,
  registrations. NGSIv2 expands nothing, so the reader writes names as they are and the import runs
  with `--importContext`; a name the context does not define is refused, by the rule above.
- **History kept outside the broker** - CrateDB or PostgreSQL tables written by a history service of
  an NGSIv2 deployment. A reader per layout, writing `temporalEntity` / `temporalInstance` records;
  where the source has no instance ids, the reader omits `instanceId` and the TRoE plugin generates them.
- **An ETSI neutral export format.** Once ETSI TC DATA defines one, a reader from it (and, for
  exports, a writer to it) - the stream already holds the same content: expanded NGSI-LD with system
  attributes.

## Not yet

- History into `--troe corDB`. corDB records its history at its own write sites and takes no events
  from the broker, so the import refuses `temporal*` records there. The design: corDB takes the import's
  events (`TroeDriver.attrEvent` / `entityEvent`) and appends them to its history log with the
  instanceId and times they carry - and does not record the current-state import as history of its
  own, as it would today.
- @contexts (hosted, cached) of the source.

## Testing

- `test/funcTests/cases/migrate_import.test` imports `test/funcTests/fixtures/migrate/orionld-stream.ndjson`
  into the suite's store (mongoc or corDB) and timescale and reads everything back through the API, then
  checks the `--importContext` rule and a second import.
- `tools/migrate/test/orionldE2E.sh` is the whole chain: it fabricates an Orion-LD deployment (MongoDB
  and PostgreSQL databases named `migtest*`), reads it with `orionldExport.py`, checks the stream is the
  functest's fixture, imports it into corDB + timescale and into mongoc + timescale, and checks both
  read back alike and as `orionldE2E.expected` says. It needs a python with `pymongo` and `psycopg`
  (`PYTHON=`), and drops only the `migtest*` databases it made.
