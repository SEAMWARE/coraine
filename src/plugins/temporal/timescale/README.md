# timescale - the TimescaleDB history plugin

`timescale.so`, loaded by `--troe timescale`. The temporal history in PostgreSQL, with TimescaleDB
hypertables when the `timescaledb` extension is installed (plain PostgreSQL otherwise), and PostGIS for
geo-queries when it can be created. A database per tenant: `<--troeName>` for the default tenant,
`<--troeName>_<tenant>` for each other one, each created through the `postgres` database the first time it
is needed and migrated to the build's schema. A request's history is queued for the tenant's writer threads,
which write it in shared transactions; it can be held in memory (8 MiB) while the database is down. Works
with any `--database`.

Also documented in: [Command-line options](../../../../doc/options.md#plugin-timescale-loaded-by-troe-timescale),
[TimescaleDB history](../../../../doc/installation.md#timescaledb-history-troe-timescale) and
[The database down](../../../../doc/installation.md#the-database-down) in Installation & Administration, its cost in
[Performance](../../../../doc/performance.md#timescaledb-the-write-path).

## Options

<!-- options of the timescale plugin: copied from doc/options.md by tools/pluginOptions.sh - edit them there -->

### `--troeHost`

`-troeHost` · `CORAINE_TROEHOST` · String · default: `localhost`
· values: a host name or IP address · plugin: timescale

The PostgreSQL server's host (libpq's `host=`).

    coraine --database corDB --troe timescale --troeHost pg --troeUser coraine

### `--troePort`

`-troePort` · `CORAINE_TROEPORT` · Int · default: `5432`
· values: 1-65535 · plugin: timescale

The PostgreSQL server's TCP port, PostgreSQL's own default.

### `--troeUser`

`-troeUser` · `CORAINE_TROEUSER` · String · default: `postgres`
· values: a PostgreSQL role · plugin: timescale

The role the broker connects as. It creates a database per tenant (`CREATE DATABASE`, through the `postgres`
database) and the tables and extensions in it, so the role needs those rights.

### `--troePwd`

`-troePwd` · `CORAINE_TROEPWD` · String · no default - no password in the connection string: libpq takes one from `PGPASSWORD` or `~/.pgpass`, if any
· values: the password · plugin: timescale

The password of `--troeUser`. A secret: `GET /admin/config` shows its value as `"***"` and marks the option
`"secret": true` (not set, the value is `null`). Given as the environment variable `CORAINE_TROEPWD`, it is
not on the command line, which any user of the machine can read (`ps`); a process's environment is
readable by its owner only. Leaving it out and giving libpq its own `PGPASSWORD` keeps it out of the
broker's options altogether. It is quoted in the connection string, as the user and `--troeName` are: a
space, a `'` or a `\` in it is passed as it is.

    $ CORAINE_TROEPWD="$(cat /run/secrets/pg)" coraine --database corDB --troe timescale --apiPlugins admin
    $ curl -s localhost:1026/admin/config     # the --troePwd entry:
    {"name": "--troePwd", "short": "-troePwd", "type": "string", "value": "***", "default": null, "from": "env", "env": "CORAINE_TROEPWD", "secret": true, ...}

### `--troeName`

`-troeName` · `CORAINE_TROENAME` · String · default: `corh`
· values: a PostgreSQL database name · plugin: timescale

The database of the default tenant's history, and the prefix of the others: tenant `t1` is in `corh_t1`.
Two brokers with different names share one PostgreSQL without sharing a history. Every broker that writes
the same history - the instances of a [High Availability](../../../../doc/high-availability.md) set, `coraine-import` - uses
the same name.

### `--troePoolSize`

`-troePoolSize` · `CORAINE_TROEPOOLSIZE` · Int · default: `10`
· values: 1-256 · plugin: timescale

The connections per tenant for the temporal reads and the Temporal API's writes (the writers have
connections of their own, `--troeWriters`). A request that finds none free waits for one, up to 10 s, and
then fails. A tenant can hold `--troePoolSize` + `--troeWriters` connections: PostgreSQL's
`max_connections` has to allow that times the tenants, times the brokers.

### `--troeWriters`

`-troeWriters` · `CORAINE_TROEWRITERS` · Int · default: `8`
· values: 0-64 · plugin: timescale

The writer threads per tenant, each on a connection of its own, opened the first time it writes. A writer
takes everything queued - up to 512 requests, 1 MiB - and writes it in one transaction; a second one starts
when none is writing, or when 8 requests wait. The queue holds 8 MiB; past it a request waits for room. `0`:
no writers - every request writes its own history, in a transaction of its own, after its response (before
it with `--troeSync`), and nothing is held while the database is down: a request whose history does not go
in loses it.

### `--troeConnectTimeout`

`-troeConnectTimeout` · `CORAINE_TROECONNECTTIMEOUT` · Int · default: `30`
· values: 0-3600 (seconds) · plugin: timescale

How long a start waits for the database, trying once a second; `0`: one try. A broker started together
with its PostgreSQL does not fail for starting first. Past it the broker does not start (exit code 1):

    $ coraine --database corDB --troe timescale --troePort 19659 --troeConnectTimeout 0
    E: ... timescale: connection to db 'postgres' failed: connection to server at "localhost" (127.0.0.1), port 19659 failed: Connection refused
    E: ... timescale: failed to initialise the default-tenant database (0 s, --troeConnectTimeout)

A password the server refuses - or none, when the server asks for one - is not retried: the start fails at
once, with the server's message. A server whose messages are not in English (`lc_messages`) is retried
until the timeout, as a server that is not up.

### `--troeDownExit`

`-troeDownExit` · `CORAINE_TROEDOWNEXIT` · Int · default: `0`
· values: 0-86400 (seconds) · plugin: timescale

What the broker does when the history database goes down while it serves. `0`: it keeps serving the current
state and its subscriptions; the history is held in memory, up to 8 MiB, retried, and written when the
database is back - past 8 MiB it is lost, and counted. With `<s>` the broker exits (code 1, at once, no
clean stop) after `<s>` seconds of the database down while history is held, or at the first row of history
that would be lost, whichever comes first. The history held is lost at the exit. A restarted broker waits
for the database (`--troeConnectTimeout`), so with an orchestrator that restarts it, an outage of the
history becomes an outage of the broker. With `--troeWriters 0` it exits at the first history lost.
[The database down](../../../../doc/installation.md#the-database-down).

    coraine --database corDB --troe timescale --troeDownExit 30

### `--troeSqlIndexes`

`-troeSqlIndexes` · `CORAINE_TROESQLINDEXES` · String · default: `none`
· values: `none`, or a comma list of `attrObserved`, `geo`, `time` · plugin: timescale

Indexes of the history beyond the basic ones, for SQL that goes to PostgreSQL directly (a dashboard, a
report); the Temporal API's queries do not use them. `attrObserved`: `troe_attrs (attr_name, observed_at)`;
`geo`: GiST on the GeoProperty values; `time`: `troe_attrs (modified_at)`. Each costs every write. Applied
at start: a chosen one is created over every row there is, one not chosen is dropped - every broker on the
same database should run with the same choice. An unknown name stops the start:
`timescale: --troeSqlIndexes: unknown index 'foo' (the choice: attrObserved, geo, time)`.
[The indexes of the history](../../../../doc/installation.md#the-indexes-of-the-history).

    coraine --database corDB --troe timescale --troeSqlIndexes attrObserved,time

### `--troeValueIndexes`

`-troeValueIndexes` · `CORAINE_TROEVALUEINDEXES` · String · no default - no declared indexes (the admin API can add some)
· values: attribute IRIs, comma-separated, or a JSON array of `{ "attribute", "type"?, "tenant"? }`; at most 32 · plugin: timescale

The attributes whose `q` value terms an index of the history serves, for temporal queries. Each declaration
is two partial indexes of `troe_attrs` (`cor_n_<id>` on `v_number`, `cor_t_<id>` on `v_text`, where
`attr_name` is the attribute, and the type when one is declared), in the database of every tenant it covers,
applied when a tenant's history is opened; an index `cor_n_*` / `cor_t_*` that no declaration names is
dropped then. Each costs the writes of its attribute's history. The format and the rules are those of
`--dbValueIndexes`; a short name stops the start: `timescale: --troeValueIndexes: 'speed' is not an IRI -
...`. Needs `COR_FEATURE_DB_INDEXES` (on by default). `/admin/troeIndexes`, and indexes made in the database:
[TimescaleDB (the history)](../../../../doc/installation.md#timescaledb-the-history).

    coraine --database corDB --troe timescale --troeValueIndexes https://example.org/speed

### `--troeInstanceCap`

`-troeCap` · `CORAINE_TROEINSTANCECAP` · Int · default: `1000000`
· values: 1-1000000 · plugin: timescale

The most instances per Attribute a temporal retrieve or query returns when it has no `lastN` or `firstN`
(§ 6.4.7.3), oldest first; more there, a `200` with the first page and a `Link: <...?offsetN=<cap>>;
rel="intervalafter"` header for the next - as corDB's option of the same name. The default is a safety
bound, not a page size.

    coraine --database corDB --troe timescale --troeInstanceCap 1000

<!-- end of the options of the timescale plugin -->
