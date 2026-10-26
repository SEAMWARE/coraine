# mongoc - the MongoDB current-state plugin

`mongoc.so`, loaded by `--database mongoc` - the default. The current state in MongoDB, through the MongoDB C
driver (libmongoc v2): a database per tenant, `<--dbName>` for the default tenant and `<--dbName>-<tenant>`
for each other one, found again at every start. The hosted JSON-LD @contexts are kept apart, in
`--globalDb`. Its options are added to the plain `coraine -u` because mongoc is the default.

Also documented in: [Command-line options](../../../../doc/options.md#plugin-mongoc-loaded-by-database-mongoc-the-default),
[Installation & Administration](../../../../doc/installation.md) - [Storage format](../../../../doc/installation.md#storage-format),
[Declared indexes](../../../../doc/installation.md#declared-indexes) - and [High Availability](../../../../doc/high-availability.md) for a
replica set.

## Options

<!-- options of the mongoc plugin: copied from doc/options.md by tools/pluginOptions.sh - edit them there -->

### `--dbHost`

`-dbHost` · `CORAINE_DBHOST` · String · default: `localhost`
· values: a host name or IP address · plugin: mongoc

The MongoDB server's host. With `--dbPort` it makes the URI `mongodb://<dbHost>:<dbPort>`. Not used when
`--dbURI` is given.

    coraine --dbHost mongo --dbPort 27018

### `--dbPort`

`-dbPort` · `CORAINE_DBPORT` · UShort · default: `27017`
· values: 1-65535 · plugin: mongoc

The MongoDB server's TCP port, MongoDB's own default. Not used when `--dbURI` is given.

### `--dbUser`

`-dbUser` · `CORAINE_DBUSER` · String · no default - no user: the connection is not authenticated
· values: a MongoDB user name · plugin: mongoc

The user the broker authenticates as. It is used only together with `--dbPwd`: both given, the URI is
`mongodb://<dbUser>:<dbPwd>@<dbHost>:<dbPort>`; a user without a password is ignored. MongoDB
authenticates it against the `admin` database (the driver's default); another authentication database
needs `--dbURI`. Not used when `--dbURI` is given.

The value goes into the URI as it is given, and the driver percent-decodes it: a user name or password with
any character outside `A-Z a-z 0-9 - . _ ~` is given percent-encoded (RFC 3986) - `@` as `%40`, `:` as
`%3A`, `/` as `%2F`, a space as `%20`, `%` itself as `%25`. The broker does not encode it: a value that
is already encoded would be encoded twice. A `:` or `/` left as it is stops the start (for a password: `mongoc: invalid
URI ...: Password must not have unescaped chars`); an `@` or a `%` left as it is gives the driver another
user, host or password than the one meant - a refused authentication, or a refused URI.

    # the password p@ss:/%
    coraine --dbHost mongo --dbUser coraine --dbPwd 'p%40ss%3A%2F%25'
    # encoding a password in the shell
    python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' 'p@ss:/%'

### `--dbPwd`

`-dbPwd` · `CORAINE_DBPWD` · String · no default - no password: `--dbUser` is ignored
· values: the password · plugin: mongoc

The password of `--dbUser`. A secret: `GET /admin/config` shows its value as `"***"` and marks the option
`"secret": true` (not set, the value is `null`). Given as the environment variable `CORAINE_DBPWD`, it is
not on the command line, which any user of the machine can read (`ps`); a process's environment is
readable by its owner only. The broker's info log (`-v`) prints the connection URI with the password as `***`.

    CORAINE_DBUSER=coraine CORAINE_DBPWD="$(cat /run/secrets/mongo)" coraine --dbHost mongo

### `--dbURI`

`-dbURI` · `CORAINE_DBURI` · String · no default - the URI is built from `--dbHost`, `--dbPort`, `--dbUser`, `--dbPwd`
· values: a MongoDB connection string (`mongodb://...`, `mongodb+srv://...`) · plugin: mongoc

The whole connection string, passed to the driver as it is: several hosts, a replica set, TLS, an
authentication database, the driver's options. Given, `--dbHost`, `--dbPort`, `--dbUser` and `--dbPwd` are
not used. The database names still come from `--dbName` and `--globalDb`, not from the URI's path.
`--high-availability mongo` needs a replica set, so a `--dbURI` with `replicaSet=`
([High Availability](../../../../doc/high-availability.md)). A secret, as a URI may carry a password: `GET /admin/config`
shows `"***"`, and the info log (`-v`) prints the URI with its password as `***`. An invalid URI stops the
start: `mongoc: invalid URI '<uri>': <the driver's reason>`.

    coraine --dbURI "mongodb://coraine:pw@m1,m2,m3/?replicaSet=rs0&authSource=admin" --high-availability mongo

### `--dbName`

`-dbName` · `CORAINE_DBNAME` · String · default: `cor`
· values: a MongoDB database name · plugin: mongoc

The database of the default tenant, and the prefix of the others: tenant `t1` is in `<dbName>-t1`. At
start the broker finds every database named `<dbName>-<something>` and sets each up as a tenant. Two
brokers with different `--dbName` share one MongoDB without seeing each other's data. The value may not be
that of `--globalDb`; the start is refused:

    $ coraine --dbName coraine
    E: ... mongoc: 'coraine' is the reserved global database name (used for JSON-LD context persistence); pick another -dbName or change --globalDb

### `--globalDb`

`-globalDb` · `CORAINE_GLOBALDB` · String · default: `coraine`
· values: a MongoDB database name, not that of `--dbName` · plugin: mongoc

The database for what belongs to no tenant: the JSON-LD @contexts the broker hosts (§ 5.13, collection
`contexts`). Kept apart from `--dbName` so that it outlives tenants and is not taken for one. Brokers that
share one MongoDB and should not share their hosted @contexts each need their own.

### `--dbTimeout`

`-dbTimeout` · `CORAINE_DBTIMEOUT` · UShort · default: `30`
· values: 1-3600 (seconds) · plugin: mongoc

How long the driver looks for a server it can use - the server-selection timeout - before an operation
fails. At start the broker pings the server and does not start if no server is found within it, so a
broker started together with its MongoDB waits for it up to this long. The same bound applies to every
operation while it runs (a server gone, an election in a replica set).

    $ coraine --dbTimeout 1          # no MongoDB running
    E: ... mongoc: ping failed: No suitable servers found: `serverSelectionTimeoutMS` expired: [Failed to connect to target host: localhost:27017]. Topology type: Single

### `--dbValueIndexes`

`-dbValueIndexes` · `CORAINE_DBVALUEINDEXES` · String · no default - no declared indexes (the admin API can add some)
· values: attribute IRIs, comma-separated, or a JSON array of `{ "attribute", "type"?, "tenant"? }`; at most 24 · plugin: mongoc

The attributes whose `q` value terms an index serves, declared at start. Each declaration is two indexes of
the `entities` collection - `cor_v_<id>` on `<attr>.@none.value` and `cor_d_<id>` on `<attr>.@datasetIds`,
partial on `type` when a type is declared - in every tenant it covers, a tenant set up later included. A
query uses them through the q pre-filter. An index named `cor_v_*` or `cor_d_*` that no declaration names
is dropped at a tenant's setup, so a declaration taken out of the option is gone at the next start.

The attributes and types are expanded (IRIs): the option has no @context. A short name stops the start,
naming the core @context's IRI of it: `mongoc: --dbValueIndexes: 'speed' is not an IRI - the option takes
attributes and entity types expanded (the core @context's: https://uri.etsi.org/ngsi-ld/default-context/speed)`.
Any other error in it stops the start too. Needs a build with `COR_FEATURE_DB_INDEXES` and
`COR_FEATURE_Q_PREFILTER` (both on by default); without them the option does not exist. What it gains and
costs, and `/admin/indexes`: [Declared indexes](../../../../doc/installation.md#declared-indexes).

    coraine --dbValueIndexes https://example.org/speed
    coraine --dbValueIndexes '[{"attribute": "https://example.org/speed", "type": "https://example.org/Vehicle"}]'

<!-- end of the options of the mongoc plugin -->
