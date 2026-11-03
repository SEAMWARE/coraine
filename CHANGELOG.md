# Changelog

Notable changes per release. The entry for a version is what its
[GitHub release](https://github.com/SEAMWARE/coraine/releases) page shows, so it is
written for someone deciding whether to upgrade — not a copy of the git log.

Versions follow [semantic versioning](https://semver.org/). Below 1.0.0 a minor
bump may still change behaviour; the entry says so where it does.

Until a release is cut, its newer entries are files in [`changelog.d/`](changelog.d/README.md),
one per pull request; `tools/changelog.sh` prints the coming release's section with them.

---

## 0.6.2 — 2026-11-04

Bug fixes above all: a batch upsert on a new tenant, a batch naming one Entity more than once in the
temporal history, the temporal API's times, a query with a filter over a registered Context Source, a
broker ended by a peer that went away, a stop that could hang on the built-in HTTP server, and the
MongoDB C driver 2.5.7 with its security fixes. Three additions beside them: a Subscription's
`notification.geometryProperty`, `GET /admin/probes`, and the `COR_OPTIMIZE=size` build for small
hardware. Two changes of behaviour to check before upgrading: a HEAD request answers 204, and
`DELETE /entities/{id}/attrs/scope` is refused (400) - the scope is changed through the Entity. The
cor libraries moved to the GitHub organisation SEAMWARE-cor.

### New

- `notification.geometryProperty` in a Subscription: the GeoProperty whose value is the `geometry` of a GeoJSON
  notification (`"accept": "application/geo+json"`), as the `geometryProperty` URL parameter is for a query.
  Without it the geometry is `location`, as before; an entity positioned through another GeoProperty had
  `"geometry": null`. Only with a GeoJSON `accept` - otherwise 400 BadRequestData. Proposed for the spec in
  TS 104 175 issue #67. ([API walkthrough](doc/api-walkthrough.md#5-subscribe))
- `GET /admin/probes` (the admin API plugin): what the code decided - which plan a query took - kept for the
  functional tests to read, in a release build too, where the traces are compiled out. Each probe set since the
  start or the last `DELETE /admin/probes`, and its last value; `GET /admin/probes/{name}` one of them.
  The first probe: `timescale.entitySelector` - `candidates` when a temporal query's `q` has a term an index serves.
  `COR_FEATURE_PROBES` (on): 2,237 bytes.
- `COR_OPTIMIZE=speed|size` (`coraine-build --optimize`): a build whose defaults spend RAM rather than
  throughput, for small hardware at low request rates - a worker per CPU whatever the store, and one
  malloc arena for the process. With mongoc on 4 CPUs the broker holds 25 MB after 100 000 entities
  created in batches of 1000, where the default build holds 197 MB. `speed` stays the default;
  `coraine --version` and `GET /build` say which a binary is.
  ([COR_OPTIMIZE](doc/building-details.md#cor_optimize))

### Faster

- The image, the CI and the `.deb` packages that bundle the MongoDB C driver (Ubuntu 24.04, Debian 13) build
  2.5.7 with four patches of the stack's: a command's retry state made only when it is retried, retryReads /
  retryWrites read once per client, a reply's error fields found in one pass, an implicit session without
  transaction state. The broker runs 9.5 % fewer instructions per `PATCH` and 7.4 % per retrieve (mongoc);
  requests/s are within their runs' spread. The patches, and the driver's licence, ship with it
  (`/usr/local/share/doc/mongo-c-driver` in the image, `/usr/share/doc/coraine/mongo-c-driver` in a package).
  Ubuntu 26.04's driver is the distribution's.
  ([Performance](doc/performance.md#the-stacks-patches-to-mongo-c-driver-257))

### Less memory

- Every thread of the broker used 256 KiB of RAM for a bridge sample buffer that only a bridge plugin's
  threads need - 18 MB with mongoc's 69 threads, before a single request. The buffer is now allocated by
  a thread at its first sample.

### Changed behaviour

- The Debian packages can be built again for a release with a fix of c³'s, without a new release: a
  package revision, version `<X>+<dist>.<N>` (`0.6.1+debian13.1`), which `apt upgrade` installs over
  `<X>+<dist>` - the broker the same, c³ at a later commit (named in `MANIFEST.txt`). The `Packages`
  workflow, run by hand, takes `ref`, `c3_ref` and `revision` for it.
  ([Install with apt](doc/installation.md#install-with-apt))
- A release is built from the libraries its pre-release campaign tested, not from each library's `main`
  at tag time: the campaign records the commit of every library (corLibs and c³ too) once, every part of
  it builds from that record, and `release.yml` builds and tags the release from the newest green
  campaign of the tag's tree - with none, it stops before publishing anything. c³'s tag must be the
  commit in the record. 0.6.0 was one commit newer in the NGSI-LD library than its campaign.
  ([The pre-release campaign](doc/testing.md#the-pre-release-campaign))
- A `COR_OPTIMIZE=size` build keeps the worker pool of a speed build (32 with mongoc) and changes only the malloc arena: with mongoc on 4 CPUs a worker per CPU saved another 1.5 MB after 100 000 entities and cost 34-46 % of the requests at 50 connections; one arena alone costs 0-3 % with mongoc, and with corDB up to 17 % of the creates (doc/building-details.md#cor_optimize)
- A HEAD request on the NGSI-LD API answers `204 No Content` when the GET would succeed, as TS 104-176 § 5
  requires ("The response code 204 No Content shall be used on success"). The GET's headers stay (`Link`,
  `NGSILD-Results-Count`, `Location`), and a 204 has no `Content-Length`. An error keeps the GET's status, with
  no body. Was `200 OK` with the GET's `Content-Length`. A HEAD on the broker's own endpoints (`/version`, ...)
  still answers what their GET does.
- `DELETE /entities/{id}/attrs/scope` answers 400 BadRequestData. Before, it removed the scope (§ 10.2.7.4).
  The scope is changed through the Entity only: Merge Entity, Update Attributes or Append with
  `"scope": "urn:ngsi-ld:null"` deletes it.
- The cor libraries, the bridges and the tools moved to the GitHub organisation
  [SEAMWARE-cor](https://github.com/SEAMWARE-cor): `github.com/SEAMWARE-cor/corNgsild`, ... coraine, c³ and
  corApisixNgsild stay in [SEAMWARE](https://github.com/SEAMWARE), as do the apt repository and the docs
  (`seamware.github.io/apt`, `seamware.github.io/docs`). The old URLs redirect, for git too. The c³ demos
  page is now `seamware-cor.github.io/c3-demos`.
- The nightly ETSI run accepts the test purposes listed in `test/etsi/deviations.txt`: a listed test that fails
  is named in the summary as a deliberate deviation and does not fail the job, and one that passes is named
  for removal from the list. The list holds the three test purposes that delete the scope with
  `DELETE /entities/{id}/attrs/scope` (013_04, 020_19_01, 020_20_01), which coraine refuses (400).
- The CI performance job (nightly and pre-release campaign) warns instead of failing: a slowdown against
  the recorded history is a warning annotation plus the job summary, and the runner's CPU model is printed.
  A flagged slowdown is measured again locally on one fixed machine, and that run decides
  (doc/testing.md, "Performance in CI").

### Fixed

- A build of a commit between releases (`0.6.1+git<date>.<sha>+<dist>`) published to the apt repository
  went into the component `main`, beside the releases, instead of `testing`.
  ([Install with apt](doc/installation.md#install-with-apt))
- On Ubuntu, a Debian package of a commit between releases sorted before the release it follows
  (`0.6.1+git<date>.<sha>+ubuntu24.04` < `0.6.1+ubuntu24.04`), so `apt upgrade` did not install it over the
  release. Its version is now `0.6.1+<dist>.<YYYYMMDD>.git<sha8>`: after the release and its package
  revisions, before the next release, on every distribution.
  ([Install with apt](doc/installation.md#install-with-apt))
- `POST /ngsi-ld/v1/entityOperations/upsert` on a tenant that does not exist yet creates the tenant, as
  Create Entity and Batch Create do - it answered 404 NonexistentTenant and wrote nothing. A query, an
  update, a PATCH or a DELETE on an unknown tenant is still a 404 NonexistentTenant.
  ([Multi-tenancy](doc/installation.md#multi-tenancy))
- A batch that names one Entity more than once - `POST /ngsi-ld/v1/entityOperations/upsert?options=update`,
  `/entityOperations/update` and `/entityOperations/merge` - records each entry in the temporal history with
  its own value and `observedAt`. Every instance carried the last entry's values, so a batch of monthly
  readings stored copies of the last one; Batch Merge's notification entries did the same. With
  `--troe corDB` only one instance was recorded, the replace-mode upsert's too.
- The temporal API renders its times as `/entities` does: to the microsecond (to the nanosecond with
  `--high-precision`, for `--troe corDB`), the fraction's trailing zeros left out. An `observedAt` sent as
  `.290` was `.29` in `/entities` and `.290` in `/temporal`; `--troe corDB` gave `observedAt` to the
  millisecond only, `--troe timescale` its `createdAt` and `modifiedAt` with all six digits (`.123000`),
  and rounded where `/entities` truncates - one microsecond apart for every other write.
  ([--high-precision](doc/options.md#--high-precision))
- A build of `main` after 0.6.1 said it was 0.6.1 (`GET /version`, the packages, the image tags) - it is
  `0.6.1+dev`, as between any two releases.
- A broker built with the built-in HTTP server (`COR_HTTP_SERVER=builtin`) could hang on SIGTERM / SIGINT that
  arrived right after it started serving: an HTTP event loop that had not started yet missed the stop and
  kept running, and the broker waited for it forever. (corHttp)
- A query with `q`, `scopeQ` or a geo-query over a registered Context Source holding a page or more of
  matching entities (20 by default) - the query an automatic EntityMap pages - counted and paged every entity
  of the type, whatever the filter said: `NGSILD-Results-Count` was the number of entities of the type, not
  of matches, and a page was cut before the filter, so `limit=1` with a match further on answered no rows.
  The rows were right. With split entities (the default) the sources cannot be given the filters; the
  broker now applies them to the entities it assembles when the EntityMap is created, so the map holds the
  matches only. ([EntityMaps](doc/installation.md#entitymaps))
- A GeoJSON notification whose `pick`, `omit` or `attributes` left out the geometry's GeoProperty (`location`
  or the `notification.geometryProperty`) had `"geometry": null`; with a `geometryProperty`, such a
  subscription was refused (400). The geometry is now taken before the projection, as for a query
  (§ 5.3.3.2); the GeoProperty is in `properties` only when the projection keeps it.
- The image, the CI and the `.deb` packages that bundle it (Ubuntu 24.04, Debian 13) are built with the MongoDB C
  driver 2.5.7 (was 2.2.2) - the fixes of its 2.3 to 2.5 releases, about 14 CVEs among them (JSON and BSON
  parsing, SCRAM and SASL, OCSP, name validation). Ubuntu 26.04's comes from the distribution, 2.2.2. 2.5 is the
  version MongoDB recommends for server 9.0.
- `troe_timescale_db_indexes.test` failed against a release build: it read a trace of the broker's SQL, compiled
  out of one. It reads the probe `timescale.entitySelector` instead.
- The image ships the MongoDB C driver's licence and third-party notices with the library (it had neither).
- coraine builds again with a compiler whose default C standard is older than C23 (gcc 13, Ubuntu
  24.04's): `src/lib/probe/probe.c` used `false` without `<stdbool.h>`, a keyword only from C23.
- A peer that goes away while the broker writes to it - an HTTP client, a WebSocket, a notification
  receiver - no longer ends the broker. The write raised SIGPIPE, whose default action ended the process:
  the broker now ignores SIGPIPE, and the write fails with EPIPE where it is made. Seen on a WebSocket
  closed while the broker sent its close frame at shutdown.
- `--ddsSyncWaitMax` defaults to a quarter of the worker threads, at least one - it was a fixed 8, meant
  as a quarter of 32 workers, so on a broker with 8 workers or fewer (corDB on a machine of 8 CPUs or
  fewer) the requests waiting for a slow or absent DDS service could take every worker and stop the
  broker answering. With 32 workers it is 8, as before.
  ([Command-line options](doc/options.md#--ddssyncwaitmax))
- The nightly's ETSI jobs passed `COR_OPTIMIZE=` empty (their matrix has no optimize), which CMake refuses: they would not have built. The makefile now refuses an empty `COR_OPTIMIZE` itself, and the valgrind size leg is gone - under valgrind malloc is valgrind's, so it ran the speed broker
- The `Allow` header of a 405 listed `HEAD` on a resource without a GET: `GET
  /temporal/entities/{id}/attrs/{attrId}` answered `Allow: DELETE, HEAD, OPTIONS`, now `DELETE, OPTIONS`.
- `GET /ngsi-ld/v1/entities/{entityId}/attrs/{attrId}` and `.../attrs/{attrId}/value` without `datasetId` return
  the default instance only (TS 104-175 § 10.4.4), and 404 when the attribute has no default instance - they
  returned every instance.
- `GET /ngsi-ld/v1/entities/{id}`, `.../attrs/{attrId}` and `.../attrs/{attrId}/value` honour `?type` on the local
  entity: a type that is none of the entity's types answers 404 ResourceNotFound (TS 104-175 § 10.4.2.4,
  § 10.4.4.4). `?type` only scoped the registrations a retrieve is forwarded to, and the local entity came back
  whatever its type.
- `GET /ngsi-ld/v1/entities/{id}?type=...` with a matching registration: a local copy of another type is no
  longer merged into the sources' Entity, and the assembled Entity must have a requested type (an auxiliary
  registration is matched by id only) - else 404. `?type=Vehicle` could answer a merged `Building`.
- A LanguageProperty whose `valueType` is not `langString` (or its IRI, `rdf:langString`) is rejected with 400
  BadRequestData on create and update (TS 104-175 § 5.2.6.4.6) - any string was accepted.
- A multi-attribute in concise format (`options=concise` - entities, queries, a single attribute,
  notifications) is an array of objects only: the default instance with nothing but its value is
  `{ "value": "Eiffel Tower" }`, no longer the bare `"Eiffel Tower"` beside the other instances' objects - as
  decided in the ETSI test-suite group (MR !304, 018_23_10). A GeoProperty instance is
  `{ "value": <geometry> }`. A bare value inside the array is still accepted on input.
- `PATCH`, `PUT`, `PUT .../value` and `DELETE` on `/entities/{id}/attrs/{id,type,scope}` answer 400
  BadRequestData, "'<name>' is an Entity member, not an Attribute". The same holds for a term of the
  request's `@context` that aliases `@id`, `@type` or `ngsi-ld:scope`. Before, PATCH and PUT on id/type gave a
  400 with a misleading detail, and `PUT .../value` gave a 404.
- A sub-attribute named `id` is refused (400 BadRequestData) on every route. Before, it was stored.
- `PATCH /entities/{id}/attrs/{attrId}` stores a concise sub-attribute (`"note": "x"`) as a Property, as
  Merge Entity does. Before, it was stored as a bare string.

---

## 0.6.1 — 2026-10-28

The same day as 0.6.0: 0.6.0 was built with an NGSI-LD library that accepts a Subscription's
`expandValues`, `jsonKeys` and `langProperties` as an array, which its broker did not read. 0.6.1 is
the broker that does, the lists as an array throughout, and `main`'s version between releases.

### New

- `expandValues`, `jsonKeys` and `langProperties` in a POST Query body and a Subscription, and `aggrMethods`
  in a temporal Query's `temporalQ`: a string, comma separated, or an array of strings with one name per
  item - an item holding a comma, an empty item or a non-string is a 400. A name is short or already
  expanded. A Subscription keeps the form it was sent in. The URL parameters stay comma separated lists.
  ([API walkthrough](doc/api-walkthrough.md#3-query))

### Changed behaviour

- Between releases, `main` reports the last release with `+dev` (`0.6.0+dev`), not the next one with
  `-dev`: the next release's number is decided when it is cut. Its Debian packages are
  `0.6.0+git<date>.<sha>` (after 0.6.0) and its images `0.6.0-post-<date>-<commit>`
  ([Installation](doc/installation.md), [Kubernetes](doc/kubernetes.md)).

### Fixed

- 0.6.0: a Subscription's `expandValues`, `jsonKeys` or `langProperties` sent as an array of strings was
  accepted and then ignored - 0.6.0's NGSI-LD library took the array, the broker read only a string. 0.6.1
  reads both forms.
  ([API walkthrough](doc/api-walkthrough.md#3-query))

---

## 0.6.0 — 2026-10-28

Two and a half weeks since 0.5.0: authorization inside the broker - the FIWARE Data Space Connector's
tokens and policies decided as the connector decides them -, Server-Sent Events, Timepoint, declared
indexes, bridges to OPC UA and CKAN and the FIWARE IoT Agents as bridge plugins, c³ - the control
center that builds, runs and shows a broker - and the stored trees in 14.5 % less RAM.

### New

- **Authorization in the broker** - `--auth`: auth plugins, and `fdsc.so`, the FIWARE Data Space Connector's
  access tokens and ODRL policies decided inside the broker as the connector decides them - 67 cases against
  a deployed connector, 0 differences; per entity, per attribute and per tenant with `--fdscNgsild`,
  notifications decided for the subscription's owner; OpenID Connect tokens (`oidc.so`); the ODRL-PAP's
  `/policy` API served by the broker (`--policyApiPort`).
  ([Authorization](doc/authorization.md))
- **An NGSI-LD plugin for APISIX** - `ngsild-pep` ([corApisixNgsild](https://github.com/SEAMWARE/corApisixNgsild)):
  `fdsc.so`'s decision in the gateway the Data Space Connector already uses, in front of any NGSI-LD broker.
- **Server-Sent Events** - a subscription on `urn:ngsi-ld:self` made on its own event stream: the `201` to its
  `POST` is `text/event-stream`, its notifications the events, and it lives as long as the stream. Over a
  WebSocket, `urn:ngsi-ld:self` is the connection.
  ([Server-Sent Events](doc/sse.md))
- **Timepoint (NGSI-LD v2.1)** - an entity, or a query's entities, as they were at a point in time, from the
  history inside corDB.
  ([Timepoint](doc/timepoint.md))
- **The OPC UA bridge** - Variables as attributes, Methods as Service Executions, the server's node tree as
  the bridge's inventory, SignAndEncrypt with certificates (`--bridges opcua`,
  [corOpcuaBridge](https://github.com/SEAMWARE/corOpcuaBridge)).
- **The CKAN bridge and DataChannels** - a CKAN portal's DataStore resources read into entities (`ckan.so`,
  [corCkanBridge](https://github.com/SEAMWARE/corCkanBridge)); a DataChannel makes each record into the entities its mapping names. A bridge reports a
  Channel's status, and why, at any time.
  ([DataChannels](doc/bridge-channels.md#datachannels))
- **The FIWARE IoT Agents as bridge plugins - prototypes** - JSON, UltraLight 2.0, LwM2M, LoRaWAN, Sigfox and
  ISOXML, each a public repository of its own (corIota*Bridge).
  ([FIWARE IoT Agents](doc/iot-agents.md))
- **What a running broker is** - `GET /admin/config` (every option it runs with, a secret masked), whether its
  build has traces (`/admin/log`), a bridge's inventory for a mapping tool
  (`/admin/bridges/{bridge}/inventory`).
- **c³, the coraine Control Center** - Compile: a broker built for a deployment from a web page (features,
  bridges, release/PGO/`make tune`/debug, on `coraine-dev`'s `coraine-build`); Operate: brokers started and
  stopped, natively or in a container, their logs, configuration and metrics, and the DDS mapping tool;
  Explore: a running broker's entities as a graph and on a map, live; the documentation, offline, and the
  same guides at [seamware.github.io/docs](https://seamware.github.io/docs/).
- c³, the control center, is in the `coraine` package (`/usr/bin/c3`); its Compile page builds brokers
  with `coraine-dev` installed. `coraine-build --source <dir>` builds from a directory of git clones
  instead of the `coraine-dev` tarball, and c³ built from clones builds from them.
  ([coraine-dev](doc/installation.md#coraine-dev-build-your-own))

- Declared indexes: the attributes whose `q` an index of the store serves - narrowed to an entity type or a
  tenant if so declared: `--dbValueIndexes` at start (the attributes' IRIs), `/admin/indexes` on a running broker; each declaration is
  an index in every tenant it covers, a tenant created later included. `mongoc`: two MongoDB indexes per
  declaration; `corDB`: a value index in the store, which a `q` on the attribute takes its candidates from,
  and `GET /admin/indexes` counts the `q` terms it served and the writes that changed it. A `q` on a declared
  attribute up to x416 (corDB) and x919 (mongoc) faster, a write of it +2.8 / +2.5 µs, a write of another
  attribute +0.13 / +0.3 µs; on corDB only the entities with a value of a declared attribute are in its index.
  Declare them at start: on corDB a change through the admin API builds every declaration's index again
  (1 000 000 entities: 0.87 s for two), the store's writes waiting.
  ([Declared indexes](doc/installation.md#declared-indexes))
- The declared indexes in `/admin/metrics`: `ngsild_index_built`, `_entries`, `_bytes`, `_accesses_total`,
  `_updates_total`, a series per declaration and tenant - what an index gains and costs on the workload, over
  time. ([In /admin/metrics](doc/installation.md#in-adminmetrics))
- Declared indexes of the TimescaleDB history: `--troeValueIndexes` at start (IRIs), `/admin/troeIndexes` on a running
  broker; each declaration is two partial indexes of `troe_attrs` (`v_number`, `v_text`) in every tenant it
  covers. The indexes made in the database that serve a `q` value term are found too (`pg_indexes`) and listed
  with `declaredBy: "database"`. A temporal query whose `q` has a term an index serves takes its entities from
  the index: a temporal query whose `q` 10 of 100 000 entities match x62 (10.2 -> 638 requests/s), a count of
  1 % of them x16; the history's writes -1.6 to -4.3 %. In `/admin/metrics`: `ngsild_troe_index_built`, `_bytes`,
  `_accesses_total`. Trace level 404 traces the selector's SQL.
  ([TimescaleDB (the history)](doc/installation.md#timescaledb-the-history))
- `--daemon` (`-bg`): the broker detaches from its terminal (fork, setsid, chdir /, stdin/stdout/stderr on
  /dev/null) and runs in the background; the command returns 0 at once. No pid file: the log names the pid,
  with or without `-v`. It needs `--logTo file` - with `--logTo stdout` or `both` the broker refuses to start.
  The foreground stays the default. ([Running the broker](doc/installation.md#running-the-broker-foreground-or-daemon-and-where-the-log-goes))
- `--logTo stdout|file|both` and `--logDir` (default `/var/log/coraine`): the log on stdout (the default, as
  before), in `<logDir>/coraine.log`, or both. The directory must exist; a log file that cannot be opened
  stops the broker at start. A `coraine.log` of an earlier start is renamed `coraine.log.old`. The file is
  written in append mode: logrotate's `copytruncate` rotates it; the broker does not reopen it (no SIGHUP).
  `CORAINE_DAEMON`, `CORAINE_LOGTO`, `CORAINE_LOGDIR`.
- corDB with `--dbDir`: a log that cannot be written - a full disk, a file-size limit, a failed `fdatasync` -
  pauses the store. Every request that writes is refused before anything is applied: 503
  `PersistenceUnavailable`, `Retry-After: 5`; GET and POST `entityOperations/query` are served. `/ready`
  answers 503 and the health report's `status` is `paused`, with a `paused` member; metrics
  `ngsild_store_paused` and `ngsild_requests_refused_store_paused_total`. The log is allocated 8 MiB ahead of
  its records, so a full disk pauses the store before a write needs the room. The broker repairs by itself -
  a snapshot of each tenant from memory, retried with a backoff up to 16 s - and the pause ends once the disk
  has room again. A failure of the history log (`--troe corDB`) is repaired too: the tenant's history is
  written anew from memory, as new segments that replace the old ones.
  ([A store that cannot write its log](doc/installation.md#a-store-that-cannot-write-its-log))
- A query with an `offset` at or past the end of its result set answers its last entity, as before, and now
  says so: `NGSILD-Warning: 199 coraine "offset 50 is past the result set (5 entities) - the last entity is
  returned"`.
  ([Pagination](doc/installation.md#an-offset-past-the-end))
- A registration PATCHed: the subordinate subscriptions of the matching subscriptions follow it (§ 10.5.2.4) -
  PATCHed at the Context Source when the registration's change narrows their entities differently, deleted when
  it no longer matches (another type, no `createSubscription`), deleted at the old endpoint and created at the new
  one when its endpoint or tenant changes, and created when it matches now. Creation and deletion of a
  registration were handled already.
  ([`--distributed`](doc/options.md#--distributed))
- Under `--high-availability mongo`, the WebSocket subscriptions of an instance that was killed or that
  crashed are deleted by the instances alive, also when it never comes back. Each instance writes the
  beat of its run in the shared database every 10 s; the subscriptions notifying a connection of a run
  with no beat for 60 s are deleted. An instance whose beat cannot be written for 60 s closes its
  WebSocket connections (status 1012). Build switch `COR_FEATURE_HA_RUNS`.
  ([High Availability](doc/high-availability.md#websocket-subscriptions-of-an-instance-gone))

### Faster

- corDB on disk: a write that updates attributes logs those attributes, not the entity - PATCH +11-13 %.
  ([Performance](doc/performance.md))
- The TimescaleDB history: TRoE writers and unnested rows, 5-11x the history write rate; the history
  database down without the current state going down (held, retried, then counted as lost); `--troeSync`
  for read-your-writes.
  ([Performance](doc/performance.md))
- `mongoc`: a `q` value term can be served by an index. It still finds a value on any instance of an
  attribute, as before; an index on `<attr>.@none.value` and one on `<attr>.@datasetIds` now serve it.
  ([q and the mongoc indexes](doc/installation.md#q-and-the-mongoc-indexes))
- Batch writes with many clients: the tree heap (corTree) keeps a cache of free blocks per thread and size,
  so threads writing the store no longer wait for each other on one mutex a size. With 50 clients on corDB
  (durable): batch update (20) +79 %, batch create (20) +186 %, batch delete (20) +225 %, merge +8 %;
  single-entity writes and reads unchanged, RSS unchanged.
  ([Threads building trees at once](doc/performance.md#threads-building-trees-at-once))

### Less memory

- The trees the broker keeps - corDB's entities, the subscription, registration and snapshot caches - take
  14.5 % less RAM: 100 000 entities of perfRun's larger store, 270 MiB → 231 MiB broker RSS. Their nodes are
  in a heap of their own, 8-byte aligned and without malloc's per-block header (−11 %), and a node that is
  not an object or an array is 32 bytes there instead of 40 (−3.6 %). On disk nothing changed: corDB's log
  and cor:// encode nodes one by one, whatever their layout in memory.
  ([The stored trees](doc/performance.md#the-stored-trees))

### Changed behaviour

- Plugins built for 0.5.x do not load in 0.6.0, of any kind: the tree node (`CorNode`) has a new layout, and
  every plugin reads nodes by their fields. The broker refuses such a plugin and exits 1, saying which and
  why; rebuild it against 0.6.0's Cor-Libs (`coraine-dev`). A plugin built for 0.6.0 is refused by a 0.5.x
  broker the same way (`undefined symbol: corNodeLayout2`).
  ([The CorNode layout](doc/plugin-architecture.md#the-cornode-layout))

- `mongoc` storage format 2: an attribute with an instance of a datasetId is marked in its document. A
  database is upgraded at the first start of 0.6.0, and 0.5.x then refuses it. Stop every broker on a
  database before starting 0.6.0 on it: a rolling upgrade from 0.5.x is not supported.
  ([Storage format](doc/installation.md#storage-format))

- `-fg` / `--foreground` is **deprecated**: the broker runs in the foreground by default, so it does
  nothing. Accepted through 0.7.x, with one warning line at start - `-fg: the broker runs in the foreground
  by default now - the option is refused from 0.8.0` - and **refused from 0.8.0**. It is no longer in
  `coraine -u`. The package's systemd unit, the Docker entry point and the scripts here no longer pass it;
  drop it from your own. `-fg` with `--daemon` is refused.
- No `--debug` / `-d` (`CORAINE_DEBUG`), for the broker and `coraine-import`: it turned on the `D` lines of
  the log, and nothing wrote one. A start with `-d` is refused as an unknown option. What shows more is the
  trace levels (`--traceLevels`). `/admin/log` has no `debug` either: `GET` no longer reports it, and
  `?debug=` is refused (400).
  ([Command-line options](doc/options.md#--tracelevels))
- A Bool option's environment variable is on for `true`, `True`, `TRUE`, `on`, `On` or `ON`, off for `false`,
  `False`, `FALSE`, `off`, `Off` or `OFF`. Any other value stops the start, the variable named
  (`coraine: CORAINE_VERBOSE=yes: not a Bool - ...`) - it was off without a word, `yes` and `1` included, and
  `on` was off too.
  ([Command-line options](doc/options.md#command-line-options))
- The core context is listed by `GET /ngsi-ld/v1/jsonldContexts` as kind `Cached`, not
  `ImplicitlyCreated` (§ 5.13.1 - a local copy of the `@context` at its external URL, compiled in instead
  of downloaded). As every `Cached` `@context`, its metadata is served (`?details=true`) and its body is not:
  `GET /ngsi-ld/v1/jsonldContexts/<core URL>` answers 422 `OperationNotSupported`.
- The transport plugin ABI is 2: a driver has a `drop` operation (every connection closed, with a status).
  A transport plugin built for ABI 1 is still loaded, without it.

### Fixed

- The image and the packages are built with libmicrohttpd 1.0.10 (was 1.0.2) - the fixes of its 1.0.3 to
  1.0.10 releases, request smuggling among them.
- A Context Source registered with a `cor://` endpoint: a forwarded query returned none of its entities
  (`[]`), and a forwarded batch took none of its per-entity answers - a batch create reported `201` for an
  entity the source refused. The same over `http://` was right, and so was a forwarded retrieve. The
  temporal query and retrieve and the other batch operations took the same path and are fixed with it.
- A default user `@context` (`--defaultUserContext`) that cannot be retrieved answers a query 504
  LdContextNotAvailable, as a create: `GET /entities?type=Room` answered 200 `[]` - `Room` expanded with the core
  context alone, in silence.
- TimescaleDB: `--troePwd`, `--troeUser` and `--troeName` go to PostgreSQL as written - each is quoted in the
  connection string. A password with a space, a `'` or a `\` was cut at its space and the rest taken as
  syntax: the start waited `--troeConnectTimeout` seconds for a database that was there, and failed.
- TimescaleDB: a password the server refuses (or none, where it asks for one) fails the start at once, with the
  server's message - it was taken for a database that cannot be reached, tried once a second for
  `--troeConnectTimeout` seconds (30). As the documentation said, and as MongoDB's (`--dbUser` / `--dbPwd`).
  ([The database down](doc/installation.md#the-database-down))
- `coraine-build --list-features` prints the switches' descriptions as they read - `"dataChannels"`, `"both"` were
  printed with CMake's backslashes, in the text list and in the `--json` one.
- A required option missing from the command line is named, the usage printed, exit 1: `coraine-import` without
  `--file` crashed (SIGSEGV; with `mongoc` after connecting). `-u` / `-U` print the option list whatever else the
  command line holds - `coraine -u -p 0` exited 1 with the range error of `--port`. The action options (`--usage`,
  `--Usage`, `--version`) have no environment variable: `-U` listed `CORAINE_VERSION`, which was never read.
- The start-up log says a size below 1 MiB in KiB: `--memoryLimit 1` logged "response size budget: 0 MiB" (it is
  64 KiB) and "writes refused above 0 MiB" (921 KiB); likewise the 403 of a response over its budget and the 503
  over the memory budget. `--healthPort` on a port that cannot be listened on says why ("Address already in use"),
  as `--corPort` does.
- `corDB`: `--dbSync`, `--dbSyncInterval`, `--dbSnapshotEvery` and `--dbCompress` without `--dbDir` were not
  looked at - `--dbSync sometimes` started, and `--dbSync request` promised a durability there was none of.
  Given without `--dbDir` they now refuse the start, each one named, as `ramDB` refuses `--dbDir`.
  ([corDB on disk](doc/installation.md#cordb-on-disk))
- Passwords and tokens no longer show in the log. They appear as `***` in mongoc's
  connection URI (`-v`, from `--dbUser`/`--dbPwd` or from `--dbURI`), in the notification and
  forwarding traces (the endpoint's `user:password` and the value of a credential header such as
  receiverInfo's `Authorization`), in the warnings that name an endpoint, in the auth plugins'
  discovery, keys and policies URLs, in the client proxy's error messages, and in a crash
  report's command line (the value after `--dbPwd`, `--dbURI` or `--troePwd`).
- `coraine -u` describes its options as [Command-line options](doc/options.md) does. Wrong before:
  `--auth` named a `none` value there is none of, `--distOpTimeout` claimed notifications and `@context`
  downloads (10 s each, not it), `--insecureNotif` only notifications and forwards (it is every outgoing
  HTTPS request, `mqtts://` too), `--httpLoops` "built-in server only" (the cor:// listener too),
  `--connectionPoolSize` "MHD thread pool" (the workers, on both servers), `--ddsSyncTimeout` left out the
  action goals' wait (504), `--asyncSnapshot` promised a status in the 201 (no body either way),
  `--fdscOrganization` showed no default (it has the PAP's); `--bridgeConfig`, `--contextSourceExtras`,
  `--subStatsFlushInterval`, `--cooldownMillis`, `--high-availability`, `--dbName`, `--dbUser`, `--dbPwd`,
  `--dbURI`, `--troeName`, `--fdscPolicies`, `--fdscOwnerMaxAge` and `--traceLevels` say their defaults
  and limits. The high-availability errors name the option as `-ha` (they said `--ha`, which is no option).
- A group of one-letter switches holding a letter that is not a switch's is an unknown option, the start
  stopped (exit 1): `coraine -vt` crashed - `-t` (`--traceLevels`) takes a value, and the group set it as
  a switch.
  ([Command-line options](doc/options.md#--tracelevels))
- `CORAINE_PORT=0 coraine -u` printed the port's range error, not the usage: a value of the environment that
  is refused is now said after the `-u`/`-U` check, as a bad value on the command line is.
  ([Command-line options](doc/options.md#command-line-options))
- The profile-guided build stops no more on gcc's fortify check (`null destination pointer`) in the metrics'
  text, which measured its length into a NULL buffer: the arm64 image is published again (none since
  2026-10-22), and the nightly's image builds (performance, the DDS bridge, the fdsc plugin vs the Data
  Space Connector) build again. It measures into a byte thrown away now; the TimescaleDB plugin's statement
  text the same.
- A Channel's `samplesIn` (`GET /ngsi-ld/v1/channels`) counts a sample before its value is written, not after
  its notifications and history: a client that saw the new value could read the count still without it.
- A request body nested deeper than 100 levels (arrays and objects) is refused with 400 `InvalidRequest`,
  detail `request body nested deeper than 100 levels`, and the broker serves the next request - a body of
  enough nested brackets, well under `--maxRequestSize`, stopped the broker.
  ([Request size](doc/installation.md#request-size))
- corDB with `--dbDir`: a write whose record cannot be put in the log - a full disk - is answered 503, with
  any `--dbSync`; it was answered as a success, and lost at the next start. After a failed `fdatasync`,
  every later write is answered 503 until the broker can write its log again (the pause, below), with any `--dbSync` - a load stops at the
  first record it cannot read; with `--dbSync request`, the writes that sync was for are answered 503 too.
  They were all answered as successes, and a `--dbSync request` write after an earlier failure no longer
  waited for its sync.
  ([`--dbSync`](doc/options.md#--dbsync))
- A distributed subscription's notification, re-dispatched from a Context Source to the subscriber, now
  carries the subscription's `notification.endpoint.receiverInfo` as headers (an `Authorization` among
  them) - it carried none, and an endpoint that requires authentication answered 401. It also carries
  the tenant (`NGSILD-Tenant`), keeps the subscription's `timeout`, and goes over the subscription's
  transport when the endpoint is not HTTP.
- The subordinate subscription created on a Context Source no longer gets the subscriber's
  `receiverInfo` and `notifierInfo` - the subscriber's credentials went to the Context Source. A PATCH of
  the subscription's `notification.endpoint` no longer moves the subordinate's endpoint from this
  broker's callback to the subscriber's.
- The `prev` link of a page past the end pointed before the offset asked for - past the end too (`offset=10`,
  5 entities, `limit=5`: `prev` at `offset=5`); it is the page before the last entity.
- An EntityMap page with an `offset` past the end of the map was empty (`[]`); it is the map's last entity, as
  for a query.
- `GET /ngsi-ld/v1/csourceRegistrations` with a filter that drops a matching registration - `q`, `scopeQ`,
  `geoQ`, a temporal query, or none (a registration with an `observationInterval` or `managementInterval`
  is then left out) - released the matches wrongly: a dropped registration stayed held, so after its
  `DELETE` its memory was never freed, and a kept one was released twice, so a request forwarding to it
  while it was deleted could read freed memory. Each match is now released once.
- A request body's numbers follow JSON's grammar: `+5`, `1.`, a bare `-` and `1.e5` are refused with 400 -
  they were taken as 5, 1, 0 and 100000. `1e400` is refused - it was stored as infinity and rendered `inf`,
  which made every response holding it invalid JSON.
- An integer past 64 bits (`12345678901234567890`) is stored as a float - it wrapped to a negative integer.
- A float is stored as the double nearest to its text: `q=a==0.3` did not find a stored `0.3` on mongoc
  (stored as 0.30000000000000004), and decimals past the 12th were dropped.
  ([Request body](doc/installation.md#request-size))
- A `Link` header (or an `@context` URL in a body) naming a `@context` the broker hosts, by the URL the
  broker gives it (`<--httpEndpoint>/ngsi-ld/v1/jsonldContexts/{id}`), is that context - read from the
  cache or the database. It was downloaded from the broker itself and cached a second time, as a `Cached`
  entry under the URL: `GET /jsonldContexts?kind=Cached` listed the Hosted context too.
- A broker without `--distributed` created subordinate subscriptions at a Context Source when a registration was
  created after a matching subscription; it creates none.
- The DDS bridge starts with a list of plain values in `dds.ddsmodule.dds`, such as
  `"whitelist-interfaces": ["192.168.1.10"]` - the DDS Enabler refused the file and the broker did not start
  (corDdsBridge). A change of the file while the broker runs reloads `allowlist`, `blocklist` and `topics`
  with such a list in it too.
  ([Bridges and Channels](doc/bridge-channels.md))

---

## 0.5.0 — 2026-10-10

Six weeks of work since 0.4.0: a broker that keeps its data on disk with no database
server, temporal history in the same process, its own binary protocol between brokers,
bridges to DDS, MQTT and Modbus, Service Execution - and everything it takes to run it
in production: arm64, Kubernetes, a migration path from Orion-LD and Debian packages.

### New

- **corDB on disk** — `--dbDir`: an append log synced every 100 ms and snapshots; the
  store survives a restart, still with no database server. A clean stop (SIGTERM) loses
  nothing.
- **Temporal history in corDB** — `--troe corDB`: the whole temporal API on history corDB
  keeps itself, on disk with the store - no PostgreSQL.
- **`cor://`** — a binary protocol for broker-to-broker traffic: forwarding without a JSON
  parse, many requests in flight on one connection.
- **Bridges and Channels** — DDS (topics both ways, services, actions; ROS 2), MQTT (device
  topics as Channels, notifications to `mqtt://`) and Modbus TCP (registers as Channels).
  coraine's own mechanism, not (yet) a standard.
- **Service Execution** — "do this" in the API: Service Descriptions, executions,
  synchronous and asynchronous (GR CIM-055).
- **Snapshots** — § 5.16 complete, writes and subscriptions on a Snapshot, on both stores.
- **The WebSocket transport** — subscriptions and notifications over a WebSocket.
- **Array reduction of entity data** — one-element arrays reduced at the input, as the
  `@context` says (`@set` / `@list`).
- **linux/arm64** — every image for amd64 and arm64 under one tag (arm64 without the DDS
  bridge); the whole functional suite runs on ARM every night.
- **Migrating from Orion-LD** — `coraine-import`, a program of its own: entities,
  subscriptions, registrations and the temporal history, with their ids and system
  timestamps (`doc/migration.md`).
- **Debian packages** — `apt install coraine` (and `coraine-dev`: the exact source and
  `coraine-build`) for Ubuntu 26.04, 24.04 and Debian 13, amd64 and arm64, from
  https://seamware.github.io/apt.
- **Kubernetes** — `--healthPort`: `/live`, `/ready` and a report on a port of its own,
  answered outside the HTTP queue; a memory budget that refuses work instead of being
  OOM-killed; a byte budget per query response (`doc/kubernetes.md`).
- **`make tune`** — a broker built for your workload: the profile trained on it, the
  allocator and the lock policy measured on it.
- **Paging that holds while the data changes** — a local query pages by position: its
  `next`/`prev` links carry the query and the place of the page's last/first entity
  (`pageAfter` / `pageBefore`), so deleted entities or entities that stop matching never make
  a page skip or repeat one, and a deep page costs what the first does. A **distributed**
  query freezes its set in an EntityMap the broker creates itself (TS 104-175 § 9.6) and
  fetches each page with one request per Context Source. `entityMap=true` freezes a local set
  on request. The map is named in the `NGSILD-EntityMap` header, which a client may also send
  instead of `?entityMap=<id>`.

### Faster

- Requests that wait run as **coroutines** of the event loops, not on worker threads.
- **Subscriptions at scale** — a write is matched only against the subscriptions that can
  match it: +32 % with 1 000 subscriptions.
- **mongoc: a query by type** reads only the page it returns (an index on
  `{type, createdAt, _id}`): ×19.7 for a type that is 1 % of the store.
- An NGSI-LD-aware JSON parser: the core terms are an enum, not strings.
- **Deep pages**: a page at depth 1 000 by position instead of offset - ×3.3 on mongoc
  (3 485 → 11 301 req/s), ×65 on corDB (243 → 15 958). A page served from an EntityMap is one
  store call (one query on mongoc instead of one per entity).

### Changed behaviour

- **`--maxResponseSize`** (new): a query stops fetching at a byte budget - entity, temporal
  and distributed queries alike: a page shorter than `limit` with a correct `next` link, a
  temporal history cut as the instance cap cuts it (TS 104-176 § 6.4.7.3 pagination), a
  Context Source's answer read only up to the budget, or 403 TooManyResults where a partial
  answer is impossible. By default 1/16 of the memory budget; **no budget outside a container with a
  memory limit**, so nothing changes there.
- **The memory budget** counts anonymous memory (RssAnon + RssShmem), not the whole resident
  set: a persistent corDB is no longer refused for the size of its memory-mapped log. New
  metric `ngsild_memory_used_bytes`.
- **`--high-availability mongo`** refuses a temporal store inside the process
  (`--troe ramDB`): each instance would hold only part of the history.
- **The Docker `HEALTHCHECK`** asks `/version` instead of `/ngsi-ld/v1/types`.
- **Temporal representation:** the entity's `modifiedAt` (timescale) includes its
  attributes' updates.
- **Subscription / registration PATCH** follows TS 104-175 § 8.4.2: a first-level member
  is replaced whole.
- **Delete Entity** with an inclusive registration: a source answering 404 is not an error
  (204).
- **mongoc** drops its old `type_1` index at startup (replaced by `{type, createdAt, _id}`).
- **Service Execution**: a service name is unique within an entity - a registration that would
  give an entity the same name twice is refused (409), and an invocation by a name two older
  registrations share answers 409 instead of picking one. `scopeQ` selects by the entity's scope
  on registrations and grouped executions. Grouped and combined executions and Combined Service
  Templates refuse a member they do not know (400) instead of ignoring it.
- **Paged queries**: a local query's `next`/`prev` links carry `pageAfter` / `pageBefore`
  instead of `offset` (a client's own `offset` works as before). A distributed query with more
  than one page is served from an automatic EntityMap: its links carry `entityMap=<id>` with
  the original query, an entity that stops matching is left out of its page, a request on a map
  with a different query is refused (400), an expired map is recreated from the request; an
  automatic map answers 200 (201 only when the client asked for one).
  `--autoEntityMaps none|distributed|all` (default `distributed`) and `--entityMapMemory`
  (default 64 MiB) bound them.
- **Downgrading to 0.4.x is not supported once 0.5.0 has written to a mongoc database.** 0.5.0
  stores the system timestamps and attribute types once per entity; 0.4.x reads attributes
  0.5.0 has written back without `type` and `createdAt`. 0.4.x cannot detect this - do not run it
  on data 0.5.0 has written.
- **A store records its storage format**, and a broker refuses to start on a store written in a newer
  format than it knows: mongoc in each database (`metadata`, `{_id: "storageFormat", version}`),
  corDB in `<--dbDir>/_storageFormat`, timescale in its schema version. A database without one (0.4.x)
  is read as it is and given 0.5.0's (`doc/installation.md`, "Storage format").
- **A DB or TRoE plugin and the broker check each other's interface stamp** at load - a hash of the
  headers they share structs through - and refuse to start on a mismatch, naming both stamps. A
  broker and plugins built from different sources no longer load and corrupt memory
  (`doc/plugin-architecture.md`, "The DB plugin interface stamp").
- **The built-in HTTP server no longer shares its port** (`SO_REUSEPORT` is off): a second broker on
  the same port fails with "Address already in use" instead of taking part of the connections.
- **`make install`** replaces each installed file (a new file renamed over the old) instead of
  writing into it: a broker running from the installed plugins keeps running.
- **`coraine-build --list-features --json`**, and one `coraine-build: phase <name>` line per phase.
- **WebSocket works like HTTP**: a connection has no tenant nor any other NGSI-LD header - every
  message carries its own in `metadata`, and a message without `NGSILD-Tenant` is for the default
  tenant. An upgrade request carrying `NGSILD-Tenant`, `Link` or `NGSILD-Snapshot` is refused (400).
- **A request in flight when the broker stops** (built-in HTTP server) is answered as failed and
  freed; a notification in flight then is counted as failed.
- **Discovery** (`/attributes`, `/attributes/{attr}`, `/types`, `/types/{type}`): a core attribute's `id`
  is its full IRI (`https://uri.etsi.org/ngsi-ld/location`, `.../observationSpace`, `.../operationSpace`,
  ...), as TS 104 175 § 5.2.6.10.1 requires; `attributeName` stays compacted.
, default `/opt/seamware/etc`): where the broker
  looks for `bridges.json` and `contextSourceExtras.json` when no file is named.

### Fixed (a selection)

- A grouped Service Execution ignored `scopeQ` and ran the service on entities outside the
  scope.
- A GeoProperty with an altitude was refused (400) by corDB built against GEOS < 3.13
  (Ubuntu 24.04); the altitude is kept.
- A request arriving in the first milliseconds after start ran outside the worker pool.
- `make release` linked debug foundation libraries; every release build now checks it
  carries no debug code.
- A `>` in a query (`q=speed>20`) broke the pagination `Link` header; link values are now
  percent-encoded.
- `?entityMap=true` copied itself into the `next` link, creating a new map on every page.
- A registration with 14 or more `contextSourceInfo` entries crashed the broker on the first
  forwarded request (a request with more than 15 headers freed memory it did not own).
- cor:// client connections were not released when a thread ended (a buffer and an open socket
  per thread).
- Periodic and re-dispatched notifications sent a `Link` header from a buffer that had gone out of
  scope.
- Allocations were not 8-byte aligned (undefined behaviour; a fault risk for atomic accesses on
  arm64).
- The built-in HTTP server leaked each thread's pool of finished coroutines when the thread ended.
- A bridge's sample text was parsed in place: the plugin's buffer came back truncated, and a string
  literal crashed the broker.
- Without `--bridgeConfig`, a bridge plugin was not given the default configuration file the
  broker itself read (MQTT: "no server").
- The MQTT and Modbus bridges read at most 64 KiB of their configuration and ignored the rest.
- A forwarded request's response (body, status text, headers) pointed into its connection's buffer,
  which could be reused by another request or freed before it was read - distributed operations,
  forwarding, Service Execution executors, snapshots and the `@context` download.
- A forwarded request whose path and query were longer than about 450 characters failed (502).
- An HTTPS request through the multi client could wait for its full timeout with its answer already
  read (data left inside OpenSSL).
- Distributed discovery counted an attribute twice when a source answered its full IRI.
- An environment-variable prefix ending in `_` gave option variables a double underscore.
- A deleted Snapshot's store was never freed (corDB), nor its geo index cache (mongoc); deleting a
  Snapshot while corDB was writing its own snapshot of the store could read freed memory.
- Coroutines and connections still waiting when the built-in server stopped were never freed.
- The image's health check asked port 1026 whatever the broker was started with: a container broker
  on another `--port` was marked unhealthy. It now asks the broker's own port (`/live` on the health
  port when there is one).

## 0.4.0 — 2026-08-28

The first tagged release. Everything below already existed and had never carried a
version anyone could ask for by name — that is what this release is for, and why
the entry describes a surface rather than a diff.

### The broker

- **NGSI-LD, in full.** ETSI GS CIM 009 v1.9.1 — entities, subscriptions,
  registrations, batch operations, the temporal API, distributed operations across
  context sources, and JSON-LD `@context` handling.
- **Notifications over HTTP, HTTPS, MQTT and MQTTS.** MQTT QoS and protocol
  version are selectable per subscription through `endpoint.notifierInfo`
  (§ 7.2), and the MQTT broker may require TLS, a username and a password.
- **Written in C**, built with CMake. One process, no runtime, no JVM.

### Plugins, loaded at startup as shared libraries

- **Current state** — `mongoc` (MongoDB) and `corDB` (in-memory).
- **Temporal history** — `timescale` (TimescaleDB), `ramdb` (in-memory) and `none`.
- **API surfaces** — `admin`, for the endpoints that are not NGSI-LD: ops and
  administration, and **56 Prometheus metrics** on `/metrics` and
  `/admin/metrics` covering entity, batch, subscription, registration, context
  source and distributed-operation activity.

Which ones a build contains is a compile-time choice, and which ones it loads is a
run-time one.

Where coraine goes from here is on the
[roadmap](https://coraine.readthedocs.io/en/latest/roadmap/).
