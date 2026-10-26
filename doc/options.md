# Command-line options

Every option of the broker, of each plugin that adds options, and of the two programs beside it -
`coraine-import` and `coraine-build`. `tools/optionsDocumented.sh` (test/funcTests, run by the functest
`options_documented.test`) fails when an option has no entry here, or an entry has no option.

**Reading an entry.** The heading is the long name; the line under it: the short name, the environment
variable, the type, the default, the values, and where the option comes from - `core` (the broker itself) or
a plugin. Then what it does, what it needs and excludes, and an example.

**Three ways to set an option**, the first found wins: the command line (`--port 1026` or `-p 1026`), the
environment variable (`CORAINE_PORT=1026` - every option has one, `CORAINE_` and the long name in capitals),
the default. A Bool environment variable is on for `true` (any case) only. A password-like option (`--dbPwd`,
`--troePwd`) is best given as its variable: it then appears in no process list.

**What a broker runs with.** `coraine -u` lists the options with their descriptions, `coraine -U` with their
variable, type and default; a plugin's options are listed when the plugin is named on the same command line
(`coraine -u --database corDB --auth oidc`). A running broker answers `GET /admin/config` with every option
it runs with, a secret masked.

**A plugin's options are separate** - each plugin has a section of its own below, after the broker's own
options, and its options are also documented with the plugin.

## The broker

- [The program](#the-program) - 9
- [Listening](#listening) - 12
- [Limits and memory](#limits-and-memory) - 7
- [Built-in HTTP server (corHttp)](#built-in-http-server-corhttp) - 1
- [libmicrohttpd HTTP server (the default build)](#libmicrohttpd-http-server-the-default-build) - 1
- [@contexts](#contexts) - 2
- [Subscriptions and notifications](#subscriptions-and-notifications) - 3
- [Distributed operations](#distributed-operations) - 5
- [High availability](#high-availability) - 1
- [Bridges](#bridges) - 5
- [The database and the history - choosing the plugins](#the-database-and-the-history-choosing-the-plugins) - 4
- [Choosing the auth plugin](#choosing-the-auth-plugin) - 1

## The plugins

- [Plugin: mongoc - loaded by `--database mongoc` (the default)](#plugin-mongoc-loaded-by-database-mongoc-the-default) - 9
- [Plugin: corDB - loaded by `--database corDB`](#plugin-cordb-loaded-by-database-cordb) - 8
- [Plugin: timescale - loaded by `--troe timescale`](#plugin-timescale-loaded-by-troe-timescale) - 12
- [Plugin: oidc - loaded by `--auth oidc`](#plugin-oidc-loaded-by-auth-oidc) - 13
- [Plugin: fdsc - loaded by `--auth fdsc`](#plugin-fdsc-loaded-by-auth-fdsc) - 13

Each plugin's entries are also in the plugin's own doc, copied from here by `tools/pluginOptions.sh` - an
entry is changed here, and CI fails while a copy differs: mongoc's
[README](https://github.com/SEAMWARE/coraine/blob/main/src/plugins/currentState/mongoc/README.md), corDB's
[README](https://github.com/SEAMWARE/corDB/blob/main/README.md#options) (written, not checked - another
repository), timescale's [README](https://github.com/SEAMWARE/coraine/blob/main/src/plugins/temporal/timescale/README.md), oidc's and fdsc's in
[Authorization](authorization.md) (§ 8.1, § 3.1).

## The programs beside the broker

- [coraine-import](#coraine-import) - 18
- [coraine-build](#coraine-build) - 13

---


## The program

What the binary says about itself, what it writes to its log, and the few switches that change how every
response is rendered. The log goes to standard output, one line per message, each line starting with its
class: `X` (fatal, the broker exits), `E` (error), `W` (warning), `I` (info), `V` (verbose), `D` (debug),
`T` (trace). `E`, `W` and `X` lines are always written; the others only when switched on. Standard
output is the default; `--logTo` and `--logDir` put the log in a file instead, or as well.

### `--usage`

`-u` · no environment variable · Bool · default: `false` (off)
· values: a switch · plugin: core (the option library, corArgs)

Prints every option, one per line as `[--long/-short <description>]`, and exits with 0. A Bool option
shows its description in `( )`, an option with a value in `< >`. The list includes the options of the
plugins named on the same command line (`--database`, `--troe`, `--apiPlugins`, `--bridges`, `--auth`),
each plugin's under a line of its own (`Database (corDB) plugin options:`), and ends with the plugin
directory. Without `--database` the list shows the options of the default store, `mongoc`.
For `--apiPlugins`, `--bridges` and `--auth` the description is the list of the plugins installed in
the plugin directory.

The other options are checked first: a value out of range ends the program with 1 and its error, not
with the list.

    coraine -u -db corDB      # the core options, then "Database (corDB) plugin options:" --dbDir, --dbSync, ...

### `--Usage`

`-U` · no environment variable · Bool · default: `false` (off)
· values: a switch · plugin: core (the option library, corArgs)

Prints every option as a table and exits with 0. The columns: the long name, the short name, the
environment variable, `Optional`, the type (`Bool`, `String`, `Int`, `UInt`, `UShort`), and where the
value in force came from (`Default Value`, `Short Option`, ...). The default value itself is not in
the table. As with `--usage`, the plugins named on the same command line add their options.

    coraine -U -db corDB      # ... --dbDir (-dbDir) CORAINE_DBDIR Optional String Default Value

### `--version`

`-V` · `CORAINE_VERSION` · Bool · default: `false` (off)
· values: a switch · plugin: core

Prints three lines and exits with 0: the program and its version, `features:` with every
compile-time feature of this binary as `NAME=1` (in the build) or `NAME=0` (left out), and
`httpServer:` with the HTTP server it was built with (`mhd` - libmicrohttpd, or `builtin` - corHttp).
It is answered before any plugin is loaded and before any other option is read, so it works on an
installation with a broken or missing plugin directory, and nothing else on the command line is
checked.

It is read from the command line only: `CORAINE_VERSION` is listed by `-U`, and setting it does
nothing - the broker starts.

    coraine -V
    # coraine 0.6.0-dev
    # features: SUBSCRIPTIONS=1 REGISTRATIONS=1 GEOQ=1 ... ICU_COLLATION=0 ...
    # httpServer: mhd

### `--verbose`

`-v` · `CORAINE_VERBOSE` · Bool · default: `false` (off)
· values: a switch; the environment variable is on for `true` (any case), off for any other value · plugin: core (the option library, corArgs)

Turns on the `V` (verbose) and the `I` (info) lines of the log: the version at start, the advertised
HTTP endpoint and where it came from, the memory budget, the response-size budget, the EntityMap
memory, the plugins loaded, `coraine running on port <port>`, and one line per request
(`Request: GET /ngsi-ld/v1/entities/...`). Without it a broker that has no error to report writes
nothing. With the `admin` API plugin it can be switched at run time (`/admin/log`).

    coraine -v -db corDB
    # V: 000000.000: coraine.c[1856]: main: coraine  0.6.0-dev
    # I: 000000.000: coraine.c[1857]: main: Advertised HTTP endpoint: http://192.168.1.138:1026 (auto-detected LAN IP)
    # ...
    # I: 000000.020: coraine.c[2280]: main: coraine running on port 1026

### `--debug`

`-d` · `CORAINE_DEBUG` · Bool · default: `false` (off)
· values: a switch · plugin: core (the option library, corArgs)

Turns on the `D` (debug) lines of the log. Neither the broker, its plugins nor its libraries write
any `D` line today, so it adds nothing to the log. With the `admin` API plugin it can be switched at
run time (`/admin/log`).

### `--traceLevels`

`-t` · `CORAINE_TRACELEVELS` · String · default: none - no trace line is written
· values: a comma-separated list of levels and ranges, `403`, `220-226,403`; levels 0-3199 · plugin: core

Turns on the `T` (trace) lines of the levels named. Each line ends with its level, `(T-403)`. A level
belongs to the code that writes it:

| Levels | Code | Defined in |
|---|---|---|
| 50-66 | the JSON parser and tree (corJson, corTree) | `corJsonTraceLevels.h`, `corTreeTraceLevels.h` |
| 100-199 | the REST layer (corRest) and the JSON-LD library (corJsonld, 150 on) | `corRestTraceLevels.h`, `corLdTraceLevels.h` |
| 200-399 | the NGSI-LD library (corNgsild): validation, forwarding (220-226), notifications (230-239), ... | `ldTraceLevels.h` |
| 400-404 | the broker: 400 forwarded requests, 401 cache sync between instances (`--high-availability`), 402 bridges, 403 Service Execution, 404 the SQL of a TRoE entity selector | `src/app/coraine/coraineTraceLevels.h` |

Traces exist in a debug build only. A release build has them compiled out: the option is accepted,
turns on nothing, and the broker says so at start
(`--traceLevels 403: this broker is a release build, its traces are compiled out - nothing to turn on`).
A trace level turns on the trace lines and nothing else - not the `V` and `I` lines of `--verbose`.
With the `admin` API plugin the levels can be changed at run time (`/admin/log`).
[Building](building-details.md) says which builds have traces.

    coraine -db corDB -t 403
    # T: 000000.000: seCoreTerms.c[72]: seCoreTermsAdd: 27 Service Execution terms added to the core context (T-403)

### `--foreground`

`-fg` · `CORAINE_FOREGROUND` · Bool · default: `false` (off)
· values: a switch; the environment variable is on for `true` (any case) · deprecated: accepted through 0.7.x, refused from 0.8.0, not listed by `-u` · plugin: core

Does nothing: the broker runs in the foreground by default. A broker started with it, or with
`CORAINE_FOREGROUND=true`, writes one warning line at start and runs as it would without it:

    W: 000000.000: coraine.c[<line>]: main: -fg: the broker runs in the foreground by default now - the option is refused from 0.8.0

With `--daemon` it is refused at start, exit code 1: `coraine: --daemon and -fg contradict each other`.
Drop it from scripts and units: from 0.8.0 the broker does not start with it.

    coraine -fg -db corDB     # the same as: coraine -db corDB, plus the warning line

### `--daemon`

`-bg` · `CORAINE_DAEMON` · Bool · default: `false` (the foreground)
· values: a switch; the environment variable is on for `true` (any case), off for any other value (`1` included) · needs: `--logTo file` · excludes: `--logTo stdout` (the default), `--logTo both`, `--foreground` · plugin: core

Runs the broker in the background: it detaches from the terminal - `fork` (the command returns 0 at
once; the child is the broker), `setsid` (a session of its own, no controlling terminal), `chdir /`,
stdin, stdout and stderr on `/dev/null`. It happens after the options are checked and the log file is
opened, so a refused combination or a log file that cannot be opened is still said on the terminal;
a start-up error after it - a port in use, a store that cannot be opened - is in the log file only,
and the exit code 0 of the command meant detached, not serving. A relative path is relative to `/`
afterwards (`--dbDir db` is `/db`); `--logDir` is opened before.

There is no pid file. The log names the pid, on a line written with or without `--verbose`:
`I: 000000.000: coraine.c[<line>]: main: --daemon: detached, pid 236587`. SIGTERM or SIGINT stop it in
order, as in the foreground; SIGHUP is not handled and ends it without that.

With `--logTo stdout` or `both` it does not start, exit code 1:
`coraine: --daemon with --logTo stdout: a daemon's stdout is /dev/null - use --logTo file`.
Not for a container (the process that detaches is the container's main process) nor for a systemd
unit of `Type=simple` (the package's) - a unit of `Type=forking` runs it.
[Installation](installation.md#running-the-broker-foreground-or-daemon-and-where-the-log-goes) has the whole picture.

    coraine --daemon --logTo file --logDir ~/coraine-log --database corDB --dbDir ~/coraine-db
    grep -- '--daemon: detached' ~/coraine-log/coraine.log      # the pid

### `--logTo`

`-logTo` · `CORAINE_LOGTO` · String · default: `stdout`
· values: `stdout`, `file`, `both` · plugin: core

Where the log goes: `stdout` - standard output, what a terminal, `docker logs` and the journal show;
`file` - the file `<logDir>/coraine.log` only (`--logDir`); `both` - each line in the file and on
stdout. Any other value is refused at start, exit code 1:
`coraine: --logTo 'syslog' is none of stdout, file, both`. With `--daemon` only `file` is accepted.

The file is not reopened while the broker runs (no SIGHUP handler); it is written in append mode, so
logrotate's `copytruncate` rotates it.

    coraine -db corDB --logTo both --logDir .      # the log on the terminal and in ./coraine.log

### `--logDir`

`-logDir` · `CORAINE_LOGDIR` · String · default: `/var/log/coraine`
· values: a directory · needs: `--logTo file` or `both` (read only then) · plugin: core

The directory of the log file, `<logDir>/coraine.log`. It is not created: it must exist and be
writable by the broker's user, else the broker does not start - exit code 1, and on stderr
`Unable to open '/var/log/coraine/coraine.log' for writing: No such file or directory`. A
`coraine.log` already there is renamed `coraine.log.old` (an earlier `.old` is replaced), and the new
file starts empty. The file is created `0664` minus the umask (`-rw-r--r--` under umask 022).

    sudo install -d -o coraine -g coraine -m 0755 /var/log/coraine
    coraine -db corDB --logTo file      # the log in /var/log/coraine/coraine.log

### `--pretty-print`

`-pp` · `CORAINE_PRETTY_PRINT` · UInt · default: `0` (compact JSON)
· values: 0-16 · plugin: core

The indentation, in spaces, of every JSON response body. 0 renders it on one line, with no
whitespace - the smallest and fastest. A request overrides it for its own response with the URL
parameter `?pretty=N` (`?pretty=0`: compact). A value over 16 is refused at start:
`coraine: value 17 for option '--pretty-print' exceeds its upper limit. Allowed range: 0-16`.

    coraine -db corDB -pp 2
    curl 'localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:T:1'            # indented by 2
    curl 'localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:T:1?pretty=0'   # {"id":"urn:ngsi-ld:T:1","type":"T",...}

### `--high-precision`

`-hp` · `CORAINE_HIGH_PRECISION` · Bool · default: `false` (6 fractional digits - microseconds)
· values: a switch · plugin: core

Renders the DateTime values `createdAt`, `modifiedAt`, `observedAt` and `expiresAt` with 9
fractional digits (nanoseconds) instead of 6 (microseconds). The broker keeps every timestamp to the
nanosecond either way; the option changes the rendering only, so it can be turned on or off on an
existing store. 6 digits is the precision NGSI-LD gives (§ 5.2.2.4). An `observedAt` a client sent
with 9 digits is answered with 6 without the option and as it was sent with it.

    coraine -db corDB -hp
    curl 'localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:T:1?options=sysAttrs'
    # ... "createdAt": "2026-10-25T19:46:07.644495761Z" ...   (without -hp: "2026-10-25T19:46:07.644495Z")

## Listening

The ports the broker listens on, and what it says about itself to others. The NGSI-LD API is on
`--port`; each of the other ports - cor://, the health probes, the policy API - is off until its
option gives it a port, and each serves only its own requests. A port that cannot be opened stops the
broker at start, with exit code 1.

### `--port`

`-p` · `CORAINE_PORT` · UShort · default: `1026`
· values: 1-65535 · plugin: core

The TCP port of the HTTP server: the NGSI-LD API, `/version`, the API plugins' paths (`/admin/...`),
and the WebSocket upgrade (`--transports ws`). It listens on every IPv4 address of the machine. 1026
is the port NGSI-LD brokers use by convention. A port another process holds stops the broker:
`X: ... main: corRestStart failed on port 1026` (libmicrohttpd's own line before it:
`corRestInit: MHD_start_daemon failed on port 1026`).

    coraine -db corDB -p 1027

### `--httpEndpoint`

`-he` · `CORAINE_HTTPENDPOINT` · String · default: `http://<the LAN address found>:<port>`, else `http://localhost:<port>`
· values: a base URL, `http://broker.example.com:1026` · plugin: core

The base URL other hosts reach this broker at. It is not checked or parsed: it is written, as it is,
into what the broker hands out for others to call back - the `Link` header of a forwarded request, the
notification endpoint of the subscriptions it makes at Context Sources (distributed subscriptions,
§ 5.8.1.4), and the URLs of the `@context` documents it hosts (the default user context of
`--defaultUserContext` among them).

Without it the broker picks an IPv4 address of its own interfaces: an interface that is up, has a
carrier, is not loopback, not point-to-point (a VPN tunnel) and not a /32 address; preferring
physical-looking names (`en*`, `eth*`, `wl*`, `em*`) and private ranges (10/8, 172.16/12,
192.168/16), and taking container and virtual interfaces (`docker*`, `veth*`, `br-*`, `tun*`,
`wg*`, `tailscale*`, ...) only when there is nothing else. With no usable interface the endpoint is
`http://localhost:<port>`. Behind a proxy, a NAT or a Kubernetes Service, the detected address is
not the one peers reach: give the option. `-v` says which endpoint is in force and where it came
from.

The authority of the endpoint (`host:port`) is also the default of `--csourceAlias`, the name the
broker gives itself in `Via` headers.

    coraine -db corDB -v --httpEndpoint http://broker.example.com:1026
    # I: ... main: Advertised HTTP endpoint: http://broker.example.com:1026 (--httpEndpoint)

### `--corPort`

`-corPort` · `CORAINE_CORPORT` · UShort · default: `0` (off - no cor:// listener)
· values: 0-65535 · plugin: core · build: `COR_FEATURE_COR_PROTOCOL`

The TCP port of the cor:// listener: the same API as the HTTP port, in the binary cor format
([The cor format and cor://](cor-protocol.md)). It listens on every address, IPv6 and IPv4, and
opens after the HTTP port, once every service is registered. It runs `--httpLoops` event loops (at
least 1, at most 16), on either HTTP server build. A port that cannot be opened stops the broker:
`cor:// cannot listen on port 7000: Address already in use`, then
`cannot listen for cor:// on port 7000`.

In the build by default; `-DCOR_FEATURE_COR_PROTOCOL=OFF` leaves it out, and the option with it.

    coraine -db corDB -v --corPort 7000
    # V: ... corRestCorListen: cor:// listening on port 7000, 4 event loops

### `--transports`

`-transports` · `CORAINE_TRANSPORTS` · String · default: none - no transport plugin, `GET /ngsi-ld/v1/ws` is not upgraded
· values: a comma-separated list of transport plugins, at most 4: `ws`; a short name or a path to the `.so` · plugin: core (loads `<plugin dir>/transport/<name>.so`) · build: `COR_FEATURE_TRANSPORTS`

Transport plugins carry the broker's API over another protocol. `ws` is the WebSocket: the HTTP port
upgrades `GET /ngsi-ld/v1/ws` to a WebSocket, which takes every request the HTTP API takes, and the
subscriptions made over it notify on it ([WebSocket](websocket.md)). Both HTTP servers serve it.
A plugin that cannot be loaded or started stops the broker:
`transport plugin 'nope' (/opt/seamware/plugins/transport/nope.so): not found`.

In the build by default; `-DCOR_FEATURE_TRANSPORTS=OFF` leaves it out, and the option with it.

    coraine -db corDB --transports ws
    curl -i localhost:1026/ngsi-ld/v1/ws -H 'Connection: Upgrade' -H 'Upgrade: websocket' \
         -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ=='
    # HTTP/1.1 101 Switching Protocols, then {"metadata":{"connection":"urn:ngsi-ld:WebSocket:446715eee556c76b-1"}}

### `--sseStreams`

`-sseStreams` · `CORAINE_SSESTREAMS` · Int · default: `1000`
· values: 0-1000000 · plugin: core · build: `COR_FEATURE_SSE`

The most Server-Sent Events streams open at a time. A stream is a subscription made with
`notification.endpoint.uri` `urn:ngsi-ld:self`: its `201` is the stream, an open connection for as long
as the subscription lives ([Server-Sent Events](sse.md)). The create that would pass the cap is refused
with 503 `InternalError`, detail
`the broker serves as many event streams as it may (--sseStreams)`. 0 refuses every stream.
Each open stream is a connection the HTTP server holds; the cap keeps their number bounded.

In the build by default; `-DCOR_FEATURE_SSE=OFF` leaves event streams out, and the option with them.

    coraine -db corDB --sseStreams 0
    curl -i -X POST localhost:1026/ngsi-ld/v1/subscriptions -H 'Content-Type: application/json' \
         -d '{"type":"Subscription","entities":[{"type":"T"}],"notification":{"endpoint":{"uri":"urn:ngsi-ld:self"}}}'
    # HTTP/1.1 503 Service Unavailable

### `--corsOrigin`

`-corsOrigin` · `CORAINE_CORSORIGIN` · String · default: none - no CORS headers
· values: an origin, `https://app.example.com`, or `__ALL` for any origin (`*`) · plugin: core

Turns CORS on, for browsers calling the broker from a page of another origin. Every response then has
`Access-Control-Allow-Origin: <the origin>` (`*` for `__ALL`) and `Access-Control-Expose-Headers:
Location, NGSILD-Results-Count, Link, NGSILD-Tenant, NGSILD-Warning, Service-Execution`. A preflight
(`OPTIONS`, answered 204 on every path) also has `Access-Control-Allow-Methods` (the methods of the
path), `Access-Control-Allow-Headers: Content-Type, Accept, Link, NGSILD-Tenant, NGSILD-Path,
Authorization, Service-Execution` and `Access-Control-Max-Age` (`--corsMaxAge`). The value is one
origin; the request's `Origin` header is not compared with it.

The policy API port (`--policyApiPort`) sends the same headers.

    coraine -db corDB --corsOrigin __ALL --corsMaxAge 600
    curl -i -X OPTIONS localhost:1026/ngsi-ld/v1/entities -H 'Origin: http://a.example' -H 'Access-Control-Request-Method: POST'
    # HTTP/1.1 204 No Content
    # Access-Control-Allow-Origin: *
    # Access-Control-Allow-Methods: GET, POST, DELETE, HEAD, OPTIONS
    # Access-Control-Max-Age: 600

### `--corsMaxAge`

`-corsMaxAge` · `CORAINE_CORSMAXAGE` · Int · default: `86400` (one day)
· values: 0-864000 (seconds) · plugin: core

The `Access-Control-Max-Age` of a preflight answer: how many seconds a browser may reuse it before it
asks again. 0 leaves the header out (the browser's own default applies). It has no effect without
`--corsOrigin`.

### `--healthPort`

`-healthPort` · `CORAINE_HEALTHPORT` · UShort · default: `0` (off - no health port)
· values: 0-65535 · plugin: core · build: `COR_FEATURE_HEALTH`

A TCP port for the health probes, served by a thread of its own, outside the HTTP server - a probe
never waits behind the requests. `GET /live`: 200 while the broker makes progress, 503 when it is
stuck (`--healthStallTimeout`). `GET /ready`: 200 once the broker serves, 503 while the store loads,
when the store does not answer, over the memory budget, stuck, or stopping. Every answer carries
a JSON report (store, TRoE, memory, requests). It listens on every address and opens before the
store is loaded, so `/live` answers during a long load. A port that cannot be opened stops the
broker: `cannot listen for the health probes on port 8080`. The probes and the report:
[Health port](installation.md#health-port); on Kubernetes: [Kubernetes](kubernetes.md#probes).

In the build by default; `-DCOR_FEATURE_HEALTH=OFF` leaves it out, and the option with it.

    coraine -db corDB --healthPort 8080
    curl localhost:8080/live
    # {"status":"ok","uptime":1,"store":{"plugin":"corDB","loaded":true,"reachable":true},...}

### `--healthStallTimeout`

`-healthStallTimeout` · `CORAINE_HEALTHSTALLTIMEOUT` · Int · default: `30`
· values: 1-3600 (seconds) · plugin: core · build: `COR_FEATURE_HEALTH`

How long a request may be in flight, with no request finishing in that time, before `GET /live` on the
health port answers 503 (and `/ready` with it). A broker with nothing to do is never stuck: it takes a
request in flight. Memory and the database never make `/live` fail. Set it above the longest request
the deployment expects - a large query, a forward to a slow Context Source (`--distOpTimeout`) - or
an orchestrator restarts a broker that is only busy. It has no effect without `--healthPort`.

    coraine -db corDB --healthPort 8080 --healthStallTimeout 120

### `--policyApiPort`

`-policyApiPort` · `CORAINE_POLICYAPIPORT` · UShort · default: `0` (off - no policy API)
· values: 0-65535 · plugin: core · build: `COR_FEATURE_POLICY_API`, libmicrohttpd builds only

A TCP port serving the ODRL-PAP's `/policy` API: `GET /policy` and `POST /policy`; `PUT`, `GET` and
`DELETE /policy/{id}`; `GET` and `DELETE /policy/odrl/{uid}`. The policies are documents of the default
tenant's collection `odrlPolicies` in the broker's store, and outlive the broker with a persistent
store. `fdsc.so` reads them as it reads the PAP's, with `--fdscPap http://127.0.0.1:<port>`
([Authorization § 7](authorization.md#7-the-policy-api---the-broker-as-its-own-pap)).

**The port has no authentication** - as the PAP has none - and the auth plugin is not asked for its
requests: whoever reaches it can change every policy. `--policyApiHost` keeps it to this machine by
default. The NGSI-LD API is not served on it, and `/policy` is not served on `--port`.

It needs a store that holds documents (corDB, ramDB, mongoc), else the broker stops:
`--policyApiPort: the store has no documents (DbDriver.doc*)`. A port that cannot be opened stops the
broker: `cannot serve the policy API on 127.0.0.1 port 8090`. It is a second HTTP listener, which only
libmicrohttpd has: a build with the built-in HTTP server leaves the policy API out, and the option
with it. `-DCOR_FEATURE_POLICY_API=OFF` leaves it out too.

    coraine -db corDB --policyApiPort 8090
    curl -i -X POST localhost:8090/policy -H 'Content-Type: application/json' \
         -d '{"odrl:uid":"https://example.org/p1","@type":"odrl:Policy"}'
    # HTTP/1.1 200 OK, Location: asgzhoiaoz
    curl localhost:8090/policy
    # [{"id":"asgzhoiaoz","odrl:uid":"https://example.org/p1","odrl":"{\"odrl:uid\":...}"}]

### `--policyApiHost`

`-policyApiHost` · `CORAINE_POLICYAPIHOST` · String · default: `127.0.0.1` (this machine only)
· values: an IP address or a host name (resolved at start) · plugin: core · build: `COR_FEATURE_POLICY_API`, libmicrohttpd builds only

The address the policy API (`--policyApiPort`) listens on. The default lets in only processes on the
same machine - the port has no authentication. In Kubernetes, where the writer of the policies
(contract management) is another pod, it is an address of the pod, with a Service of its own for the
port and a NetworkPolicy that lets in only that writer. A name that does not resolve stops the broker:
`corRestListen: 'no.such.host': Name or service not known`, then
`cannot serve the policy API on no.such.host port 8090`. It has no effect without `--policyApiPort`.

    coraine -db corDB --policyApiPort 8090 --policyApiHost 0.0.0.0

### `--apiPlugins`

`-api` · `CORAINE_APIPLUGINS` · String · default: none - no API plugin, no `/admin` paths
· values: a comma-separated list of API plugins, at most 16: `admin`; a short name or a path to the `.so` · plugin: core (loads `<plugin dir>/api/<name>.so`)

API plugins add HTTP paths of their own next to the NGSI-LD API, on `--port`. `admin` serves
`/admin/version`, `/admin/health`, `/admin/config` (every option with its value, its default and where
it came from, a secret masked), `/admin/log` (`--verbose`, `--debug`, `--traceLevels` at run time),
`/admin/metrics`, `/admin/tenants`, `/admin/plugins`, `/admin/subStats/flush`, `/admin/indexes`,
`/admin/troeIndexes`, `/admin/troe/dump` and `/admin/bridges/{bridge}/inventory`
([Plugin architecture](plugin-architecture.md)). A plugin that cannot be loaded stops the broker, with
the usage after the error: `API plugin 'nope' (/opt/seamware/plugins/api/nope.so): not found`. A short
name is looked for under the plugin directory (`/opt/seamware/plugins`, moved by
`SEAMWARE_PLUGIN_DIR`); a name with a `/` is a path.

    coraine -db corDB -api admin
    curl localhost:1026/admin/config
    # {"options":[{"name":"--verbose","short":"-v","type":"bool","value":false,"default":false,"from":"default",...},...]}

## Limits and memory

What a request may cost the broker, and how the broker shares its threads and memory among requests.
A limit reached answers the request that reached it - 413, 403 `TooManyResults` or 503 - and the
broker goes on serving the others.

### `--maxRequestSize`

`-mrs` · `CORAINE_MAXREQUESTSIZE` · Int · default: `2` (MiB)
· values: 0-4096 (MiB); 0: no cap · plugin: core

The largest request body, in MiB. A larger one is refused with 413 `RequestEntityTooLarge`
(NGSI-LD § 6.3.2), detail `request body of 1200081 bytes exceeds broker limit of 1048576 bytes`, before
it is parsed. 0 removes the cap. The URL and the headers are not part of it: they are bounded by the
HTTP server (`--httpConnectionMemory` with libmicrohttpd).
[Request size](installation.md#request-size).

    coraine -db corDB -mrs 1
    curl -i -X POST localhost:1026/ngsi-ld/v1/entities -H 'Content-Type: application/json' --data-binary @big.json
    # HTTP/1.1 413 Content Too Large

### `--maxResponseSize`

`-maxResponseSize` · `CORAINE_MAXRESPONSESIZE` · Int · default: `-1` (1/16 of the memory budget; none without a memory budget)
· values: -1 to 4096 (MiB); 0: no budget · plugin: core · build: `COR_FEATURE_RESPONSE_BUDGET`

The byte budget, in MiB, of an entity query: `GET /ngsi-ld/v1/entities`, `POST
/ngsi-ld/v1/entityOperations/query`, the temporal queries and what Context Sources answer to a forwarded
one. The store counts each entity as it fetches it and stops before the one that would pass the budget,
so a query's memory is bounded while it is fetched. The page then ends early, with `Link`
`rel="next"`; 403 `TooManyResults` when the first entity alone is larger than the budget, or when the
query needs every match at once (`orderBy`) and they are larger.

-1, the default, is 1/16 of the memory budget (`--memoryLimit`, or 85 % of the container's limit): a
response holds more than its bytes, and several run at once. Outside a container and without
`--memoryLimit` there is no budget. `-v` says which budget is in force.
[Response size](installation.md#response-size).

In the build by default; `-DCOR_FEATURE_RESPONSE_BUDGET=OFF` leaves it out, and the option with it.

    coraine -db corDB -v -mrs 4 --maxResponseSize 1
    # V: ... main: response size budget: 1 MiB (--maxResponseSize)
    curl -i 'localhost:1026/ngsi-ld/v1/entities?type=T&orderBy=id'    # with 1.2 MB of entities of type T
    # HTTP/1.1 403 Forbidden  {"type":"https://uri.etsi.org/ngsi-ld/errors/TooManyResults",...}

### `--memoryLimit`

`-memoryLimit` · `CORAINE_MEMORYLIMIT` · Int · default: `0` (85 % of the container's memory limit; none outside a container)
· values: 0-1048576 (MiB) · plugin: core

The memory budget, in MiB. Over 90 % of it the requests that grow memory (POST, PUT, PATCH) are
refused; over all of it everything is refused except deletes and monitoring (`/version`, `/metrics`,
`/admin/*`). A refused request gets 503, `Retry-After: 5` and the error type
`https://coraine.readthedocs.io/errors/MemoryBudgetExceeded`. The memory compared is the process's
anonymous and shared memory, measured every 100 ms; the executable, the libraries and corDB's
memory-mapped log are not counted.

0, the default, takes 85 % of the smallest cgroup memory limit the process lives under (the
container's, a Kubernetes pod's): the broker turns requests away before the kernel kills it at the
limit. Outside a container, with no limit, there is no budget and nothing is checked. The budget is
also the base of `--maxResponseSize`'s default. [Memory budget](installation.md#memory-budget).

    coraine -db corDB -v --memoryLimit 512
    # V: ... memoryBudgetInit: memory budget: 512 MiB (writes refused above 460 MiB)

### `--connectionPoolSize`

`-cps` · `CORAINE_CONNECTIONPOOLSIZE` · Int · default: `0` (the CPUs the process may run on, for a store that never waits; 32 otherwise)
· values: 0-200 · plugin: core

The number of worker threads that run requests. With libmicrohttpd it is also the number of the
server's I/O threads (its thread pool); with the built-in server it sizes the connection pool, 16
connections per worker and at least 1024.

0 decides from the store: a request that waits - on MongoDB, on TimescaleDB, on a forwarded request -
holds its worker for the wait, so a store that waits gets 32. A store that never waits (`corDB` or
`ramDB`, with `--troe none` or `corDB`, and no `--distributed`) gets one worker per CPU the process may
run on (its CPU affinity and cgroup quota, not the host's count): more workers than CPUs only take
turns on them. `/admin/config` shows the number in force.

    coraine -db corDB -cps 4

### `--noInline`

`-noInline` · `CORAINE_NOINLINE` · Bool · default: `false` (inline dispatch on, where it applies)
· values: a switch · plugin: core

Hands every request to a worker thread. By default, with a store that never waits (`corDB` or `ramDB`,
with `--troe none` or `corDB`), a request of the `/ngsi-ld/v1/entities` family that waits on nothing -
no forward, no wait for a bridge service, its `@context` already cached - runs on the I/O thread that
read it, without the two thread switches of the hand-off: about twice the throughput for retrieves and
small queries ([Where a request runs](performance.md#where-a-request-runs)). With any other store
nothing runs inline, and the option changes nothing. It is there for measurements, and as a way out.
Metrics: `ngsild_requests_inline_total`, `ngsild_requests_worker_total`.

    coraine -db corDB -v --noInline
    # V: ... inlineDispatchInit: inline dispatch: off (--noInline) - every request is handed to a worker

### `--cooldownMillis`

`-cms` · `CORAINE_COOLDOWNMILLIS` · Int · default: `30000` (30 seconds)
· values: 0-86400000 (milliseconds); 0: no default cooldown · plugin: core

After a notification to an endpoint fails, how long, in milliseconds, the subscription sends nothing
more to it: the notifications in that window are dropped, not sent and not queued. It is the default
for a subscription or a context source subscription that does not give `notification.endpoint.cooldown`
itself (§ 5.2.15), and applies to change-driven and periodic notifications alike. 0 leaves a
subscription with no cooldown of its own without any. A registration's forwards are not affected: they
cool down only by the registration's own `management.cooldown`.

    coraine -db corDB --cooldownMillis 0
    # three changes, the endpoint down: timesSent 3, timesFailed 3 (with the default 30000: 1 and 1)

### `--entityMapMemory`

`-entityMapMemory` · `CORAINE_ENTITYMAPMEMORY` · Int · default: `64` (MiB)
· values: 0-65536 (MiB); 0: no cap, and no automatic EntityMaps · plugin: core

The memory all EntityMaps may hold together, in MiB - an entity id and its sources, about 70 bytes plus
the id each. When a new map does not fit, the expired maps go first, then the automatic maps used
longest ago; a map a client asked for is never evicted. An automatic map that still does not fit is
not made (the query is paginated without one); a map a client asked for (`?entityMap=true`, `POST
/ngsi-ld/v1/entityMaps`) is refused with 403 `TooManyResults`. 0 removes the cap and turns the
automatic maps off. Metrics: `ngsild_entity_map_bytes`, `ngsild_entity_map_store_size`.
[EntityMaps](installation.md#entitymaps).

    coraine -db corDB -v --entityMapMemory 0
    # V: ... main: EntityMap memory: 0 MiB (no cap, no automatic EntityMaps)

## Built-in HTTP server (corHttp)

Options that act on a broker built with the built-in HTTP server (`cmake -DCOR_HTTP_SERVER=builtin`;
`coraine -V` says `httpServer: builtin`). A build with libmicrohttpd (the default, `httpServer: mhd`)
accepts them, so a deployment does not have to know which server its binary carries
([Choosing the HTTP server](building-details.md#choosing-the-http-server)).

### `--httpLoops`

`-hl` · `CORAINE_HTTPLOOPS` · Int · default: `0` (one per CPU the process may run on, at most 4)
· values: 0-64 · plugin: core (acted on by the built-in HTTP server and by the cor:// listener)

The number of event loops sharing the HTTP port, each on a thread of its own. 0 takes one per CPU the
process may run on (its CPU affinity and cgroup quota), at most 4: four loops on eight cores scale
nearly linearly and keep the tail latency under half a millisecond, eight add 16 % throughput for seven
times the p99; more loops than CPUs is slower than one. A value given is used as it is, up to 64.

With libmicrohttpd the HTTP server has no loops of the broker's and ignores it. On either build, the
cor:// listener (`--corPort`) runs this many loops, at most 16. `/admin/config` shows the number in
force.

    coraine -db corDB -hl 1

## libmicrohttpd HTTP server (the default build)

Options that act on a broker built with libmicrohttpd (`cmake -DCOR_HTTP_SERVER=mhd`, the default;
`coraine -V` says `httpServer: mhd`). A build with the built-in HTTP server accepts them and ignores
them.

### `--httpConnectionMemory`

`-hcm` · `CORAINE_HTTPCONNECTIONMEMORY` · Int · default: `12` (kB)
· values: 4-1024 (kB) · plugin: core (acted on by the libmicrohttpd HTTP server)

The memory libmicrohttpd gives each connection, in kB: a request's line and headers - the method, the
URL with its query, every header - must fit in it. At 12 kB that is headers up to about 11 kB and a URL
up to about 9 kB; at 32 kB (libmicrohttpd's own default) more than 20 kB. A request that does not fit is
refused by the server itself, before it reaches the broker, with its own short HTML body: 431 for its
headers, 414 for its URL. The log says so, at most once a second:
`a request was refused with 414 by the HTTP server: its URL exceeds the connection memory (4 kB, --httpConnectionMemory) - a long query fits in the body of POST /ngsi-ld/v1/entityOperations/query; or raise --httpConnectionMemory`.

The memory is cleared after every request, so it is a cost on every request: 12 kB instead of 32 kB is
worth 2-3 % of a core on small requests
([The HTTP server's connection memory](performance.md#the-http-servers-connection-memory)). A long
query fits in the body of `POST /ngsi-ld/v1/entityOperations/query`, which has no URL limit; a large
access token (1.5-4 kB) fits in 12 kB with the other headers. [Request size](installation.md#request-size).

    coraine -db corDB -hcm 4
    curl -i "localhost:1026/ngsi-ld/v1/entities?id=$(seq -f 'urn:ngsi-ld:T:%05g' -s, 1 300)"
    # HTTP/1.1 414 URI Too Long

## @contexts

The JSON-LD side of the broker's identity: the @context a request without one of its own is expanded
with, and the free-form JSON the broker adds to its own description on `/ngsi-ld/v1/info/sourceIdentity`.

### `--defaultUserContext`

`-duc` · `CORAINE_DEFAULTUSERCONTEXT` · String · no default - a request without a user @context is expanded with the core context alone
· values: a URL (`http://`, `https://`), a file path, or `file://<path>` · plugin: core

The @context used wherever no user @context is given: a request without a `Link` header (and, for
`application/ld+json`, without `@context` in the body), the bridge configuration read at startup, a
sample arriving on a bridge transport, the auth plugin's expansion of policy terms. The core context
still sits last in the chain and wins term by term, so the default context only adds terms.
A response to a request that brought no @context names it in its `Link` header.

A URL is kept as given and downloaded the first time it is needed, like any other @context - it is not
checked at start. A request that needs it while it cannot be downloaded is answered `504`
`LdContextNotAvailable` (`unable to retrieve @context from '<url>'`).

A file is read and parsed at start, then hosted by the broker as
`<advertised endpoint>/ngsi-ld/v1/jsonldContexts/defaultUserContext` (kind `Hosted`), and that URL is the
default. The advertised endpoint is `--httpEndpoint`, else the auto-detected LAN address. The file
must be a JSON object with an `@context` member that is an object or an array. Refused at start:

    X: ... main: --defaultUserContext: the file '<path>' cannot be read
    X: ... main: --defaultUserContext: '<path>' is not a JSON-LD @context document - a JSON object with "@context" (an object or an array)
    X: ... main: --defaultUserContext: the @context in '<path>' cannot be used

The DDS mapping tool's `dds-context.jsonld` is such a file ([DDS and ROS 2](dds.md)).

    coraine --database corDB --defaultUserContext my-context.jsonld
    curl localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:Room:1
    # Link: <http://192.168.1.138:1026/ngsi-ld/v1/jsonldContexts/defaultUserContext>; rel="http://www.w3.org/ns/json-ld#context"; ...

### `--contextSourceExtras`

`-csx` · `CORAINE_CONTEXTSOURCEEXTRAS` · String · default: `/opt/seamware/etc/contextSourceExtras.json` (`$SEAMWARE_ETC_DIR/contextSourceExtras.json` when `SEAMWARE_ETC_DIR` is set)
· values: the path of a JSON file, at most 1 MiB · plugin: core

A JSON file whose content `GET /ngsi-ld/v1/info/sourceIdentity` renders as `contextSourceExtras`
(NGSI-LD § 5.2.40), verbatim: it is never expanded as JSON-LD. Read once, at start. Any JSON value is
accepted.

The default file is the one the installation writes (version, git SHA and build time of the build);
when it is missing, nothing is rendered and there is no error. A file named with the option must be
there and valid - refused at start:

    X: ... contextSourceExtrasLoad: --contextSourceExtras: cannot open '<path>'
    X: ... contextSourceExtrasLoad: contextSourceExtras: '<path>' empty or too large (max 1 MiB)
    X: ... contextSourceExtrasLoad: contextSourceExtras: '<path>' is not valid JSON

(The size and JSON checks apply to the default file too.)

    echo '{"site":"plant-3","operator":"ops@example.org"}' > extras.json
    coraine --database corDB --contextSourceExtras extras.json
    curl localhost:1026/ngsi-ld/v1/info/sourceIdentity
    # {..., "contextSourceExtras":{"site":"plant-3","operator":"ops@example.org"}}

## Subscriptions and notifications

How notifications are filtered and delivered, and how often the delivery statistics of the
subscriptions are written to the database.

### `--notifyValueChangeOnly`

`-nvco` · `CORAINE_NOTIFYVALUECHANGEONLY` · Bool · default: `false`
· values: a flag · plugin: core

Suppresses the notification of an entity update that changed no attribute value. An update notifies
only when an attribute was created or deleted, or when the `value` of an attribute (of any of its
datasetId instances, or an instance added or removed) differs from what it was. A change to a
sub-attribute (`unitCode`, `observedAt`, ...) or to the timestamps alone does not notify. Creations
and deletions always notify. Off by default: NGSI-LD notifies every update that matches.

    coraine --database corDB --notifyValueChangeOnly
    # PATCH {"temperature":21} on a temperature of 21               -> no notification
    # PATCH {"temperature":{"value":21,"unitCode":"CEL"}}            -> no notification
    # PATCH {"temperature":22}                                       -> notification

### `--insecureNotif`

`-insecureNotif` · `CORAINE_INSECURENOTIF` · Bool · default: `false`
· values: a flag · plugin: core

Turns off the certificate verification of the broker's outgoing TLS connections, so an `https://`
endpoint with a self-signed (or otherwise unverifiable) certificate is accepted. By default the peer is
verified against the system CA store, and a notification to such an endpoint fails (the subscription's
`status` becomes `failed`). The setting is process-wide: it covers every outgoing HTTPS request of
the broker - notifications, requests forwarded to Context Sources, @context downloads - not
notifications alone. With `--bridges mqtt` it also accepts the certificate of an `mqtts://` endpoint
([The MQTT bridge](mqtt-bridge.md)). Meant for endpoints inside a trusted network.

    coraine --database corDB --insecureNotif

### `--subStatsFlushInterval`

`-ssfi` · `CORAINE_SUBSTATSFLUSHINTERVAL` · Int · default: `60`
· values: 0-86400 (seconds) · plugin: core

The delivery statistics of every subscription (`timesSent`, `timesFailed`, `lastNotification`,
`lastSuccess`, `lastFailure`) are counted in RAM and written to the database every this many seconds,
for every tenant - the regular subscriptions, the subscriptions on Context Source registrations, the
periodic ones, and the subscriptions on Snapshots. `0` writes them only when asked:
`POST /admin/subStats/flush` (`--apiPlugins admin`) does it at any time. A `GET` of a subscription
answers from RAM, so it is always current; the interval decides how much of the counting a restart
loses - the shutdown does not write them.

    coraine --database corDB --subStatsFlushInterval 10

## Distributed operations

Forwarding to the Context Sources registered with `POST /ngsi-ld/v1/csourceRegistrations`: whether it
happens, how long a source is waited for, how the broker names itself to other brokers, how the
parts of an entity held by several sources are put together, and how such queries are paged. See
[Response size - Distributed queries](installation.md#distributed-queries) and
[EntityMaps](installation.md#entitymaps).

### `--distributed`

`-dist` · `CORAINE_DISTRIBUTED` · Bool · default: `false`
· values: a flag · plugin: core

Turns forwarding on. With it, every entity operation is matched against the registrations and
forwarded to the Context Sources that match; `GET /types` and `/attributes` are forwarded too; a
subscription whose entities a registration covers is also created on that source (§ 5.8.1.4), with
this broker's advertised endpoint as the callback.

Off by default: the matching costs a registration lookup on every entity operation, and most
deployments hold their data locally. Without it the Registry API (create, update, delete, query and
discover registrations, § 12) works the same, and nothing is forwarded - registrations can be made
before forwarding is turned on. With it, and at least one registration, a request to
`/ngsi-ld/v1/entities` always runs on a worker thread (never inline on the I/O thread).

    coraine --database corDB --distributed
    # a registration of type Room at http://localhost:19622, the Room there:
    # GET /ngsi-ld/v1/entities/urn:ngsi-ld:Room:7  -> 200 (without --distributed: 404)

### `--distOpTimeout`

`-dtmo` · `CORAINE_DISTOPTIMEOUT` · Int · default: `5000`
· values: 1-600000 (milliseconds) · plugin: core

How long the broker waits for the answer to a request it forwards to a Context Source whose
registration has no `timeout` of its own (§ 5.2.34 - a registration's `timeout` wins). The forwards of
one query run in parallel and wait together, for the longest `timeout` among their registrations, or
this value when none has one. A source that does not answer in time is left out of the answer, which
says so in an `NGSILD-Warning` header. It is also the default of any other outgoing HTTP request that
sets no timeout of its own. Not covered: notifications (10 s, or the subscription's
`endpoint.timeout`) and @context downloads (10 s). The time to connect is a separate 5 s.

    coraine --database corDB --distributed --distOpTimeout 1000
    # a source that takes 3 s: the query answers after 1 s with its local matches and
    # NGSILD-Warning: 199 127.0.0.1:19632 "No response was received from the registration endpoint within the timeout period"

### `--csourceAlias`

`-csourceAlias` · `CORAINE_CSOURCEALIAS` · String · no default - the authority (`host:port`) of the advertised endpoint: `--httpEndpoint`, else the auto-detected LAN address; `<program name>:<port>` when the endpoint has none
· values: a name, unique per broker · plugin: core

The broker's `contextSourceAlias` (§ 5.7.5): the name it adds to the `Via` header of every request it
forwards, and finds in the `Via` header of a request that has already passed through it. It is
also the `contextSourceAlias` on `GET /ngsi-ld/v1/info/sourceIdentity` and the end of its `id`
(`urn:ngsi-ld:ContextSource:<alias>`). With an `NGSILD-Tenant` it is `<alias>:<tenant>`.

Loop detection treats a registration whose source answers with this broker's alias as pointing back at
this broker, and does not forward to it. So the alias must differ between brokers that forward to each
other: two brokers with the same alias never forward to one another.

    coraine --database corDB --csourceAlias broker-a
    curl localhost:1026/ngsi-ld/v1/info/sourceIdentity -H 'NGSILD-Tenant: t1'
    # {"id":"urn:ngsi-ld:ContextSource:broker-a:t1", ..., "contextSourceAlias":"broker-a:t1", ...}

### `--noSplitEntities`

`-noSplitEntities` · `CORAINE_NOSPLITENTITIES` · Bool · default: `false`
· values: a flag · plugin: core

Changes how a distributed query treats an entity held by several sources. By default (split entities,
standard NGSI-LD) the broker collects the ids of the matches from every source, assembles each entity
from all its parts, and applies the query's filters to the assembled entity. With `--noSplitEntities`
each entity is taken as lying wholly at one source: the query is forwarded as it is, with its `offset`
and `limit`, the answers are merged, and an entity that comes from more than one place is kept once -
the first answer that has it, the local store first. The URL parameter `?splitEntities=true|false`
decides for one request. Needs `--distributed` to have any effect.

    coraine --database corDB --distributed --noSplitEntities
    # Room:7 with humidity here and temperature at the source:
    #   default:            [{"id":"urn:ngsi-ld:Room:7", "humidity":..., "temperature":...}]
    #   --noSplitEntities:  [{"id":"urn:ngsi-ld:Room:7", "humidity":...}]

### `--autoEntityMaps`

`-autoEntityMaps` · `CORAINE_AUTOENTITYMAPS` · String · default: `distributed`
· values: `none`, `distributed`, `all` · plugin: core · build: `COR_FEATURE_AUTO_ENTITY_MAP`

Which `GET /ngsi-ld/v1/entities` queries get an EntityMap that the client did not ask for, when their
first page (`offset` 0) has more after it. The map's URI comes in `NGSILD-EntityMap`, and the
`Link` `rel="next"` pages through it.

- `distributed` - the distributed queries only: the sources are not asked for `offset` / `limit`, so
  without a map there is no correct second page.
- `all` - local queries too. A local query pages in the store already (`pageAfter`), and a map makes
  its first page cost much more ([performance.md](performance.md#what-automatic-entitymaps-cost)).
- `none` - a map only when the client asks for one (`?entityMap=true`).

`--entityMapMemory 0` turns automatic EntityMaps off whatever this says. Any other word is refused at
start:

    X: ... main: --autoEntityMaps: '<word>' is none of none, distributed, all

The details - what is not mapped, the headers, the sources' own maps - are in
[EntityMaps](installation.md#entitymaps). A build with `-DCOR_FEATURE_AUTO_ENTITY_MAP=OFF` accepts the
option and ignores it.

    coraine --database corDB --autoEntityMaps all
    # GET /ngsi-ld/v1/entities?type=Room&limit=2 with 3 Rooms:
    # NGSILD-EntityMap: /ngsi-ld/v1/entityMaps/urn:ngsi-ld:EntityMap:dd741ef40001

## High availability

Several broker instances behind a load balancer, sharing one database: each keeps its subscriptions,
registrations and @contexts cached in RAM, and this keeps those caches in step with what the other
instances write. Everything about the setup is in [High Availability](high-availability.md).

### `--high-availability`

`-ha` · `CORAINE_HIGH_AVAILABILITY` · String · no default - off: each instance knows only what it wrote itself and what it loaded at start
· values: `mongo` (an `<ip:port>` is accepted by the parser and refused at start) · plugin: core (`mongo` needs the `mongoc` database plugin)

`mongo`: the instance watches the whole MongoDB deployment with one change stream, opened at start
before the caches are loaded, and applies every subscription, registration and @context change it
reports. Snapshots are not synchronised.

Refused at start:

- a database plugin that cannot report another instance's writes (`corDB`, `ramDB`):

      X: ... haInit: --ha mongo needs a database plugin that can report what another broker instance wrote; the 'corDB' plugin cannot. Point the broker at a shared database, or use '--ha <ip:port>' once the haaux server exists

- a temporal store inside the process (`--troe corDB`, `--troe ramDB`); `--troe timescale` and
  `--troe none` are accepted:

      X: ... haInit: --ha mongo needs a temporal store shared by every broker instance; '--troe corDB' keeps the history inside each broker, ...

- a MongoDB that is not a replica set, or a change stream that cannot be opened (the privileges -
  [High Availability - Requirements](high-availability.md#requirements)), then
  `X: ... main: unable to start the HA channel 'mongo'`.

Any other value is taken as the address of a haaux server, which does not exist yet, and is refused:

    X: ... haInit: --ha 10.0.0.1:7000: an address means the haaux server, which is not implemented yet. Use '--ha mongo'

    coraine --dbURI "mongodb://db1,db2,db3/?replicaSet=rs0" --high-availability mongo

## Bridges

Bridges connect the broker to peers that do not speak NGSI-LD (DDS and ROS 2, MQTT, Modbus, CKAN, ...),
each one a plugin. The options here load them, name their configuration file, and set how a write that
calls a service on the other side waits for its reply. The configuration file itself is documented
with each bridge: [DDS and ROS 2](dds.md), [The MQTT bridge](mqtt-bridge.md),
[The Modbus bridge](modbus-bridge.md), and the bridge repositories
([corDdsBridge](https://github.com/SEAMWARE/corDdsBridge), [corMqttBridge](https://github.com/SEAMWARE/corMqttBridge),
[corModbusBridge](https://github.com/SEAMWARE/corModbusBridge), [corCkanBridge](https://github.com/SEAMWARE/corCkanBridge)).

### `--bridges`

`-br` · `CORAINE_BRIDGES` · String · no default - no bridge is loaded
· values: a comma-separated list of bridge plugins, at most 8 - short names resolve to `/opt/seamware/plugins/bridge/<name>.so` (`$SEAMWARE_PLUGIN_DIR/bridge/<name>.so`), a name with a `/` is a path · plugin: core

Loads the bridge plugins. `coraine -u` lists the ones installed (`ckan`, `dds`, `loopback`, `modbus`,
`mqtt`; `loopback` has no transport and serves the tests). A plugin's own options are added to the
command line and to `coraine -u` under `Bridge (<name>) plugin options:`. The transports are opened last
in the startup, once everything else is up. `GET /version` lists the loaded bridges under `bridges`.
With `mqtt`, subscriptions can notify `mqtt://` and `mqtts://` endpoints. Refused at start - a plugin
that is not there, or whose `init` fails:

    bridge plugin 'nosuch' (/opt/seamware/plugins/bridge/nosuch.so): not found
    X: ... bridgesInit: init failed for bridge plugin '<name>'

    coraine --database corDB --bridges dds,mqtt --bridgeConfig /etc/coraine/bridges.json

### `--bridgeConfig`

`-brc` · `CORAINE_BRIDGECONFIG` · String · default: `/opt/seamware/etc/bridges.json` if it exists (`$SEAMWARE_ETC_DIR/bridges.json` when `SEAMWARE_ETC_DIR` is set)
· values: the path of a JSON file, at most 4 MiB · plugin: core

The bridge configuration file: the Channels (which endpoint of which bridge maps to which entity
attribute) and each bridge's own settings. It is read at start, and its path is handed to every loaded
bridge. A file named with the option and missing stops the broker; the default file missing means no
Channels from a file, and no error. Without `--bridges` the option is not used at all - not even
checked. Refused at start:

    X: ... channelConfigLoad: --bridgeConfig: cannot read '<path>'
    X: ... channelConfigLoad: the bridge configuration '<path>' is empty or improbably large

    coraine --database corDB --bridges dds --bridgeConfig dds.json

### `--ddsSync`

`-ddsSync` · `CORAINE_DDSSYNC` · Bool · default: `false`
· values: a flag · plugin: core

Makes waiting for the reply the default of a write to a service attribute (an attribute that calls a
service on a bridge: a DDS or ROS 2 service, for one). A waiting write calls the service before the
value is stored, waits up to `--ddsSyncTimeout` for the reply, and stores value and reply together. A
reply that does not come in time: the value is stored, the answer is `202`, and the reply lands in the
attribute when it comes. Without waiting, the service is called after the write, the answer is the
write's own, and the reply lands later.

The URL parameter `?ddsSync=true` or `?ddsSync=false` decides for one request, whatever this option
says. Only the three entity PATCH forms act on it, and only for a PATCH of one attribute; every other
route accepts the parameter and ignores it. Any other value than `true` / `false` is `400`
(`'ddsSync' must be 'true' or 'false'`), when the broker has a Channel that writes out. Not DDS-only:
it applies to the services of every bridge. A waiting request holds a worker thread; `--ddsSyncWaitMax`
caps how many do. Action goals do not depend on it: they always wait (see `--ddsSyncTimeout`).

    coraine --database corDB --bridges dds --ddsSync
    # PATCH /ngsi-ld/v1/entities/{id}/attrs?ddsSync=false   - this one does not wait

### `--ddsSyncTimeout`

`-ddsSyncTimeout` · `CORAINE_DDSSYNCTIMEOUT` · Int · default: `0` - the bridge configuration's `syncTimeoutMs`, else 200
· values: 0-600000 (milliseconds) · plugin: core

How long a bridge request waits for the other side. A waiting write to a service attribute
(`--ddsSync`, `?ddsSync=true`) waits this long for the reply, then answers `202` and lets the reply land
when it comes. A write to an action attribute - on every route, with or without `ddsSync` - waits this
long for the goal to be accepted or rejected; not answered in time, nothing is written, the answer is
`504` (`goalNotAnswered`), and the goal is cancelled.

`0` (the default) takes the `syncTimeoutMs` of the bridge configuration file (the DDS bridge's
`dds.ngsild.syncTimeoutMs`, as Orion-LD reads it), and 200 when the file has none. A value given here
wins over the file. `GET /admin/config` shows the value in use: `200` by default.

    coraine --database corDB --bridges dds --ddsSyncTimeout 500

### `--ddsSyncWaitMax`

`-ddsSyncWaitMax` · `CORAINE_DDSSYNCWAITMAX` · Int · default: `8`
· values: 0-200 · plugin: core

The most requests that wait for a service's reply at the same time. A waiting request holds one of
the worker threads (`--connectionPoolSize`, 32 by default), so a slow or absent DDS network could
otherwise take every worker and stop the broker answering. When every slot is taken, the next
request calls the service without waiting and answers `202`. `0`: no request ever waits. The default
of 8 is a quarter of the default 32 workers.

    coraine --database corDB --bridges dds --ddsSync --ddsSyncWaitMax 16

## The database and the history - choosing the plugins

Two plugins hold what the broker stores: the **current-state** plugin (`--database`) keeps the Entities,
Subscriptions, Registrations and Snapshots, and the **TRoE** plugin (`--troe`) keeps the temporal history.
Both are chosen at start and fixed for the life of the process. The options of the chosen plugins are
added to the command line only when the plugin is loaded: `coraine --database corDB -u` and
`coraine --troe timescale -u` list them. `coraine-import` takes the same store options, by the same names
([Migrating a database to coraine](migration.md)). Where plugins are found:
[Plugin architecture](plugin-architecture.md#where-plugins-are-loaded-from).

### `--database`

`-db` · `CORAINE_DATABASE` · String · default: `mongoc`
· values: `mongoc`, `corDB`, `ramDB` (the plugins installed in `<plugin dir>/db/currentState/`), or a path to a `.so` · plugin: core

The current-state store. A short name is resolved to `<plugin dir>/db/currentState/<name>.so`; the plugin
dir is `/opt/seamware/plugins` unless `SEAMWARE_PLUGIN_DIR` says otherwise. An argument with a `/` in it is
taken as the path of the plugin itself. `coraine -u` lists the short names it finds in the plugin dir.

- `mongoc` - MongoDB ([Plugin: mongoc](#plugin-mongoc-loaded-by-database-mongoc-the-default)).
  The default, so a broker started without the option needs a MongoDB server.
- `corDB` - the store in the broker's own memory, in RAM or also on disk with `--dbDir`
  ([Plugin: corDB](#plugin-cordb-loaded-by-database-cordb)).
- `ramDB` - corDB built without the disk: a restart starts empty
  ([ramDB](installation.md#ramdb-cordb-in-ram-only)).

A plugin that is not there stops the start, with the usage after the error:

    $ coraine --database bogus
    DB plugin 'bogus' (/opt/seamware/plugins/db/currentState/bogus.so): not found

An option of another plugin is an unknown option: `coraine --database ramDB --dbDir /x` answers
`coraine: unrecognized option: --dbDir`.

    coraine --database corDB                      # in RAM, no external service
    coraine --database corDB --dbDir /var/lib/coraine

### `--troe`

`-troe` · `CORAINE_TROE` · String · default: `none`
· values: `none`, `timescale`, `corDB`, `ramDB`, or a path to a `.so` · plugin: core

The temporal history (TRoE - Temporal Representation of Entities): what the Temporal API
(`/ngsi-ld/v1/temporal/...`) reads. A short name is resolved to `<plugin dir>/troe/temporal/<name>.so`;
`corDB` is the exception.

- `none` - no history. The temporal API has nothing to answer from.
- `timescale` - PostgreSQL, with TimescaleDB when the extension is there, a database per tenant
  ([Plugin: timescale](#plugin-timescale-loaded-by-troe-timescale)). Works with any `--database`.
- `corDB` - the history inside the corDB store: the same lock, the same log, on disk with `--dbDir`. It is
  not a plugin file of its own: it is the current-state plugin's, so it needs `--database corDB`. With
  another store the start is refused:

      $ coraine --database ramDB --troe corDB
      TRoE 'corDB' keeps the history inside the corDB store - it needs --database corDB (not 'ramDB')

- `ramDB` - a ring of the latest 256 TRoE events in RAM, read through `GET /admin/troe/dump`; a test
  instrument, not a history the Temporal API serves.

A plugin that is not there stops the start:
`TRoE plugin 'bogus' (/opt/seamware/plugins/troe/temporal/bogus.so): not found`.

    coraine --database corDB --troe corDB --dbDir /var/lib/coraine   # one process, state + history on disk
    coraine --database corDB --troe timescale --troeHost pg

### `--troeSync`

`-troeSync` · `CORAINE_TROESYNC` · Bool · default: `false`
· values: a flag · plugin: core

When a request's history is handed to the TRoE plugin. By default after the response is sent: the
response does not carry the cost, and a temporal read sent right after the response may not see the
write yet. With `--troeSync` the history is handed over before the response, once the service routine is
done, so a temporal read that follows the response sees it. The response still does not wait for the
database to commit: with `timescale` the rows are queued for the writers, and a temporal read of the
tenant waits for every row queued before it.

It changes nothing with `--troe corDB` (the store records the history at the write itself, under its
lock) or `--troe none`. Cost with `timescale`: 1-6 % fewer writes a second
([Performance](performance.md#-troesync-handed-over-before-the-response)).

    coraine --database corDB --troe timescale --troeSync

### `--asyncSnapshot`

`-asyncSnapshot` · `CORAINE_ASYNCSNAPSHOT` · Bool · default: `false`
· values: a flag · plugin: core

How `POST /ngsi-ld/v1/snapshots` runs a Snapshot's `snapshotQueries` and `snapshotTemporalQueries`.
By default inside the request: the `201 Created` comes once the capture is done, and the Snapshot's
`snapshotStatus` is final (`success`, `partial`, `empty` or `failure`). With `--asyncSnapshot` the capture
runs on a thread of its own (§ 5.16.1.4: "executed in the background"): the `201` comes at once, the
Snapshot reads `"snapshotStatus": "preparing"` until the capture is done, and the SnapshotNotification is
sent from that thread. The `201` has no body either way. Any `--database`.

    coraine --database corDB --asyncSnapshot

## Choosing the auth plugin

Without an auth plugin every request is let in. `--auth` loads one, and from then on the plugin decides, for
every request, who sent it and whether it may: an HTTP request, a cor:// request, and each message of a
transport (WebSocket). Not asked: the health port (`--healthPort`), the policy API port (`--policyApiPort`),
an in-process forward and the broker's own requests. A plugin's own options exist only when it is loaded:
`coraine --auth oidc -u` and `coraine --auth fdsc -u` list them. Built with `COR_FEATURE_AUTH` (on by
default); off, `--auth` and both plugins are not in the build. Also documented in:
[Authorization](authorization.md).

### `--auth`

`-auth` · `CORAINE_AUTH` · String · default: no default - no plugin, every request is let in
· values: `oidc`, `fdsc` (the plugins in `<plugin dir>/auth/`), or a path to a `.so` · plugin: core · build: `COR_FEATURE_AUTH`

The auth plugin. A short name is resolved to `<plugin dir>/auth/<name>.so` (`/opt/seamware/plugins` unless
`SEAMWARE_PLUGIN_DIR` says otherwise); an argument with a `/` in it is the plugin's path. `coraine -u` lists
the names it finds there.

- `oidc` - the access tokens of an OpenID provider (Keycloak, Auth0, Entra ID, ...), and two roles in them
  ([Plugin: oidc](#plugin-oidc-loaded-by-auth-oidc)).
- `fdsc` - the FIWARE Data Space Connector's tokens and its ODRL policies
  ([Plugin: fdsc](#plugin-fdsc-loaded-by-auth-fdsc)).

The broker fails closed: a plugin that is not there, or whose options its start-up refuses, stops the broker
with exit code 1 - `--auth` asks for security, and a broker started without it would be open.

    $ coraine --auth none
    auth plugin 'none' (/opt/seamware/plugins/auth/none.so): not found
    $ coraine --database corDB --auth oidc
    E: ... authInit: auth plugin 'oidc': no provider: --oidcIssuer (a token's iss is always checked)
    the auth plugin did not start - see the log

There is no `none` value: leaving `--auth` out is what lets every request in. A plugin option without its
plugin is an unknown option: `coraine --oidcIssuer x` answers `coraine: unrecognized option: --oidcIssuer`.

While a plugin has no keys yet (and `fdsc`: no policies), it answers 503 rather than let a request in. A 401
carries `WWW-Authenticate: Bearer`, with `resource_metadata` pointing at
`GET /.well-known/oauth-protected-resource` - how a client gets a token the plugin takes
([Authorization](authorization.md), § 2.5).

    coraine --database corDB --auth oidc --oidcIssuer https://keycloak.example.org/realms/city
    coraine --database corDB --auth /opt/seamware/plugins/auth/oidc.so --oidcIssuer https://keycloak.example.org/realms/city

---

## Plugin: mongoc - loaded by `--database mongoc` (the default)

The current state in MongoDB, through the MongoDB C driver (libmongoc v2): a database per tenant,
`<--dbName>` for the default tenant and `<--dbName>-<tenant>` for each other one, found again at every
start. The hosted JSON-LD @contexts are kept apart, in `--globalDb`. Every option here is the mongoc
plugin's own; the options are added to the plain `coraine -u` because mongoc is the default.

Also documented in: [Installation & Administration](installation.md) - [Storage format](installation.md#storage-format),
[Declared indexes](installation.md#declared-indexes), and [High Availability](high-availability.md) for a replica set.

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
([High Availability](high-availability.md)). A secret, as a URI may carry a password: `GET /admin/config`
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
costs, and `/admin/indexes`: [Declared indexes](installation.md#declared-indexes).

    coraine --dbValueIndexes https://example.org/speed
    coraine --dbValueIndexes '[{"attribute": "https://example.org/speed", "type": "https://example.org/Vehicle"}]'

## Plugin: corDB - loaded by `--database corDB`

The current state in the broker's own memory: one store per tenant, a tree of the Entities, Subscriptions
and Registrations with its indexes, behind a read-write lock. Without `--dbDir` it is in RAM only and a
restart starts empty. With `--dbDir` every write also appends its effect to a memory-mapped log, synced
on a timer, with a snapshot as the log grows: a clean stop, `kill -9`, a crash or an OOM kill lose nothing
that was acknowledged, a machine that dies at most the last sync interval. With `--troe corDB` the same
store keeps the history. The plugin lives in the corDB library, which also builds **ramDB**
(`--database ramDB`): the same store without the disk, whose only options are `--dbLockPrefer` and
`--dbValueIndexes` - the disk options are unknown options there, and `--troe corDB` is refused.

Also documented in: corDB's [README](https://github.com/SEAMWARE/corDB#readme) and
[persistence design](https://github.com/SEAMWARE/corDB/blob/main/doc/persistence.md);
[corDB on disk](installation.md#cordb-on-disk) and its cost in [Performance](performance.md#cordb-on-disk-what-persistence-costs).

### `--dbDir`

`-dbDir` · `CORAINE_DBDIR` · String · no default - the store is in RAM only, and a restart starts empty
· values: a directory path · plugin: corDB

The directory the store is kept in, created (with its parents) if missing. Inside: `_storageFormat` (the
[storage format](installation.md#storage-format), a number as text) and one directory per tenant, `_` for
the default one, each with its log segments (`log-<N>.cor`) and its latest snapshot (`snap-<N>.cor`). At
start every tenant directory is loaded - the snapshot, then the log after it. A store written by a newer
release (a storage format above this build's) stops the start and is left as it is. A directory that cannot
be created stops the start: `corDB: --dbDir '<dir>': <reason>`. With `--troe corDB` the history is kept here
too. `--dbSync`, `--dbSyncInterval`, `--dbSnapshotEvery` and `--dbCompress` act only with it.

    coraine --database corDB --dbDir /var/lib/coraine
    # a POST, a stop, a start on the same directory: the Entity is back
    $ ls /var/lib/coraine /var/lib/coraine/_
    /var/lib/coraine:
    _  _storageFormat

    /var/lib/coraine/_:
    log-000001.cor  snap-000001.cor

### `--dbSync`

`-dbSync` · `CORAINE_DBSYNC` · String · default: `interval`
· values: `interval`, `request`, `none` · plugin: corDB

When a write reaches the disk. A write copies its record into the memory-mapped log - the kernel's page
cache - before its response; this option decides when that is `fdatasync`ed.

- `interval` - a flusher thread syncs every tenant's log every `--dbSyncInterval` ms. The response does not
  wait. A broker that dies loses nothing it acknowledged; a machine that dies, at most the last interval.
- `request` - the response waits for its record's sync. The writers that arrive during one sync share the
  next (group commit). A write that cannot be logged (a full disk) is answered 503.
- `none` - the log is written, never synced: for tests, benchmarks, a RAM disk.

Given without `--dbDir` it stops the start: `corDB: --dbSync without --dbDir - the store is in RAM only, there is no disk for it to act on`.

An unknown value stops the start - with `--dbDir` only, as without it the option is not read:
`corDB: --dbSync 'sometimes': interval, request or none`.

    coraine --database corDB --dbDir /var/lib/coraine --dbSync request

### `--dbSyncInterval`

`-dbSyncInterval` · `CORAINE_DBSYNCINTERVAL` · Int · default: `100`
· values: 1-60000 (milliseconds) · plugin: corDB

The time between two rounds of the flusher, which writes and syncs every tenant's log. With
`--dbSync interval` it bounds what a machine that dies can lose; with `--dbSync request` a waiting write
starts a round at once, and the interval is the longest the log goes unsynced without one. A value out of
range stops the start: `coraine: value 0 for option '--dbSyncInterval' is inferior to its lower limit.
Allowed range: 1-60000`. Given without `--dbDir` it stops the start: `corDB: --dbSyncInterval without --dbDir - the store is in RAM only, there is no disk for it to act on`.

### `--dbSnapshotEvery`

`-dbSnapshotEvery` · `CORAINE_DBSNAPSHOTEVERY` · Int · default: `64`
· values: 1-65536 (MiB) · plugin: corDB

How much log a tenant writes before it is snapshotted: once it has written this many MiB of log **and** at
least as much log as its last snapshot was big, so that the snapshots of a growing store cost in proportion
to what is written. A snapshot is written on a thread of its own, in slices under the read lock; the log
before it is then deleted, and a start replays only the log after it. Every tenant is also snapshotted at a
clean stop (SIGTERM, SIGINT). Smaller: faster starts, more snapshot work while running.
Given without `--dbDir` it stops the start: `corDB: --dbSnapshotEvery without --dbDir - the store is in RAM only, there is no disk for it to act on`.

### `--dbCompress`

`-dbCompress` · `CORAINE_DBCOMPRESS` · Bool · default: `false`
· values: a flag · plugin: corDB

Compresses the snapshots and the finished log and history segments with zstd (level 3), on the snapshot
thread; the open segment, which writes append to, never. With it a segment rolls at 64 MiB instead of
1 GiB, so a history - which no snapshot drops - is mostly compressed segments. A start reads a compressed
store with the option or without it, and a start with it compresses the finished segments a run without it
left. libzstd (`libzstd.so.1`) is loaded only then; missing, the start is refused: `corDB: --dbCompress needs
libzstd.so.1 (zstd) - not found`. Given without `--dbDir` it stops the start: `corDB: --dbCompress without --dbDir - the store is in RAM only, there is no disk for it to act on`. On disk, 100 000 Entities: 74 MB plain, 1.4 MB
compressed ([persistence § 6a](https://github.com/SEAMWARE/corDB/blob/main/doc/persistence.md#6a---dbcompress)).

    coraine --database corDB --dbDir /var/lib/coraine --dbCompress

### `--dbLockPrefer`

`-dbLockPrefer` · `CORAINE_DBLOCKPREFER` · String · default: `reads`
· values: `reads`, `writes` · plugin: corDB (and ramDB)

Whom a tenant's store lock lets in first when readers and writers both wait. `reads` is glibc's default
read-write lock; `writes` makes it writer-preferring. Measured: writers first, a bare `PATCH` +2-4 %, a
`PATCH` that notifies -3-4 % (the match and the notification read the Entity)
([Performance](performance.md#writes-that-notify-and-which-writer-the-lock-lets-in-first)). Another value
stops the start: `corDB: --dbLockPrefer 'both': reads or writes`.

    coraine --database corDB --dbLockPrefer writes

### `--dbValueIndexes`

`-dbValueIndexes` · `CORAINE_DBVALUEINDEXES` · String · no default - no declared indexes (the admin API can add some)
· values: attribute IRIs, comma-separated, or a JSON array of `{ "attribute", "type"?, "tenant"? }`; at most 64 · plugin: corDB (and ramDB)

The attributes whose `q` value terms an index serves, declared at start. Each declaration is a value index
in the store of every tenant it covers - the attribute's values, each with its Entity, in order - serving
`==`, a range, a list, `<`, `<=`, `>` and `>=`. Not on disk: built by the first query that can use it, again
after every start; a change of the declarations (the admin API) builds every index again. The format, the
expanded names, and the start refused for a short name (`corDB: --dbValueIndexes: 'speed' is not an IRI -
...`) are as for mongoc's option of the same name. Needs a build with `COR_FEATURE_DB_INDEXES` (on by
default). [Declared indexes](installation.md#declared-indexes),
[corDB's README](https://github.com/SEAMWARE/corDB#queries-by-value---declared-indexes).

    coraine --database corDB --dbValueIndexes https://example.org/speed

### `--troeInstanceCap`

`-troeCap` · `CORAINE_TROEINSTANCECAP` · Int · default: `1000000`
· values: 1-1000000 · plugin: corDB, with `--troe corDB`

The most instances per Attribute a temporal retrieve or query returns when it has no `lastN` or `firstN`
(§ 6.4.7.3), oldest first. More there: the response is a `200` with the first page and a
`Link: <...?offsetN=<cap>>; rel="intervalafter"` header for the next. The default is a safety bound, not a
page size. Present only with `--troe corDB` (timescale has an option of the same name).

    $ coraine --database corDB --troe corDB --troeInstanceCap 2
    # an Entity created with speed 1, then PATCHed to 2, 3, 4
    $ curl -i localhost:1026/ngsi-ld/v1/temporal/entities/urn:ngsi-ld:T:capx1
    HTTP/1.1 200 OK
    Link: </ngsi-ld/v1/temporal/entities/urn:ngsi-ld:T:capx1?offsetN=2>;rel="intervalafter";type="application/json"

    {"id":"urn:ngsi-ld:T:capx1","type":"T","speed":[{"type":"Property","value":1,...},{"type":"Property","value":2,...}]}

## Plugin: timescale - loaded by `--troe timescale`

The temporal history in PostgreSQL, with TimescaleDB hypertables when the `timescaledb` extension is
installed (plain PostgreSQL otherwise), and PostGIS for geo-queries when it can be created. A database per
tenant: `<--troeName>` for the default tenant, `<--troeName>_<tenant>` for each other one (every run of
characters outside `a-z0-9` in the tenant's name becomes one `_`), each created through the `postgres`
database the first time it is needed and migrated to the build's schema. The default tenant's is set up at
start, before the broker serves. A request's history is queued for the tenant's writer threads, which write
it in shared transactions; it can be held in memory (8 MiB) while the database is down. Works with any
`--database`.

Also documented in: [TimescaleDB history](installation.md#timescaledb-history-troe-timescale) and
[The database down](installation.md#the-database-down) in Installation & Administration; its cost in
[Performance](performance.md#timescaledb-the-write-path).

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
broker's options altogether. It goes into the connection string unquoted: a password with a space or a `'`
in it is not passed as it is.

    $ CORAINE_TROEPWD="$(cat /run/secrets/pg)" coraine --database corDB --troe timescale --apiPlugins admin
    $ curl -s localhost:1026/admin/config     # the --troePwd entry:
    {"name": "--troePwd", "short": "-troePwd", "type": "string", "value": "***", "default": null, "from": "env", "env": "CORAINE_TROEPWD", "secret": true, ...}

### `--troeName`

`-troeName` · `CORAINE_TROENAME` · String · default: `corh`
· values: a PostgreSQL database name · plugin: timescale

The database of the default tenant's history, and the prefix of the others: tenant `t1` is in `corh_t1`.
Two brokers with different names share one PostgreSQL without sharing a history. Every broker that writes
the same history - the instances of a [High Availability](high-availability.md) set, `coraine-import` - uses
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

A wrong password is retried the same way, until the timeout.

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
[The database down](installation.md#the-database-down).

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
[The indexes of the history](installation.md#the-indexes-of-the-history).

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
[TimescaleDB (the history)](installation.md#timescaledb-the-history).

    coraine --database corDB --troe timescale --troeValueIndexes https://example.org/speed

### `--troeInstanceCap`

`-troeCap` · `CORAINE_TROEINSTANCECAP` · Int · default: `1000000`
· values: 1-1000000 · plugin: timescale

The most instances per Attribute a temporal retrieve or query returns when it has no `lastN` or `firstN`
(§ 6.4.7.3), oldest first; more there, a `200` with the first page and a `Link: <...?offsetN=<cap>>;
rel="intervalafter"` header for the next - as corDB's option of the same name. The default is a safety
bound, not a page size.

    coraine --database corDB --troe timescale --troeInstanceCap 1000

## Plugin: oidc - loaded by `--auth oidc`

`oidc.so` takes the access tokens of an OpenID provider - a JWT the provider signed with RS256 or ES256 -
and decides each request at its admission, on its method and NGSI-LD operation and the token's roles and
tenants. Two roles: the read role reads (`GET`, `HEAD`, `OPTIONS` and the three query operations that are
POSTs), the write role reads and writes. No token, or one that does not hold: 401; a token without the role
or the tenant a request needs: 403. The provider's keys are fetched by a thread of the plugin; until there
are some, every request is a 503. Built with `COR_FEATURE_AUTH_OIDC` (on by default). Also documented in:
[Authorization](authorization.md#81-options).

### `--oidcIssuer`

`-oidcIssuer` · `CORAINE_OIDCISSUER` · String · default: no default - the broker does not start
· values: the provider's issuer URL · plugin: oidc

The provider's issuer: the `iss` every token must have, compared as a string. Without `--oidcDiscovery` or
`--oidcJwks` the keys are found through `<issuer>/.well-known/openid-configuration` (a trailing `/` of the
issuer is dropped first). Required - without it the broker stops at start:
`auth plugin 'oidc': no provider: --oidcIssuer (a token's iss is always checked)`.

A token of another issuer is a 401 with `error_description="wrong issuer"`. The issuer is what
`GET /.well-known/oauth-protected-resource` gives as `authorization_servers`.

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city

### `--oidcDiscovery`

`-oidcDiscovery` · `CORAINE_OIDCDISCOVERY` · String · default: no default - `<--oidcIssuer>/.well-known/openid-configuration`
· values: a URL (`http://`, `https://`), or a file (a path, or `file://`) · plugin: oidc

The provider's OpenID discovery document, for a broker that reaches the provider by another name than the
issuer's (inside a cluster: `http://keycloak:8080/...` while the tokens carry the public name). Its
`jwks_uri` gives the keys, and its `issuer` must be `--oidcIssuer` (OpenID Connect Discovery 1.0 § 4.3): a
document of another issuer is not used, the log says so
(`its issuer is '...', not --oidcIssuer's '...'`), and with no keys every request is a 503. The `jwks_uri`
in it must be one the broker can reach.

Not with `--oidcJwks`: the broker stops at start with
`auth plugin 'oidc': --oidcDiscovery and --oidcJwks: one or the other`.

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city \
            --oidcDiscovery http://keycloak:8080/realms/city/.well-known/openid-configuration

### `--oidcJwks`

`-oidcJwks` · `CORAINE_OIDCJWKS` · String · default: no default - the keys come from the discovery document
· values: a URL (`http://`, `https://`), or a file (a path, or `file://`) · plugin: oidc

The provider's JWK Set itself, instead of the discovery document: no discovery document is read.
`--oidcIssuer` is still required (the `iss` of a token is always checked). Not with `--oidcDiscovery`. A
JWK Set that cannot be read is in the log
(`oidc: the provider's keys (<where>): cannot open '<file>'`), and until it can every request is a 503
(`the provider's keys are not loaded yet`). A file is read again every `--oidcRefresh` seconds, as a URL is
fetched.

    coraine --database corDB --auth oidc --oidcIssuer https://idp.example.org/realms/city --oidcJwks /etc/coraine/jwks.json

### `--oidcAudience`

`-oidcAudience` · `CORAINE_OIDCAUDIENCE` · String · default: no default - `aud` is not checked
· values: an audience (a client id) · plugin: oidc

An audience the token's `aud` must hold (a string, or an array that has it). A token without it is a 401
with `error_description="wrong audience"`. Without the option every token the provider issued for any of
its clients is taken, and the broker says so at start
(`oidc: --oidcAudience not set - a token issued for any client of the provider is taken`). Keycloak puts a
client into `aud` only with an audience mapper on the client the applications log in with
([Authorization](authorization.md#82-with-keycloak)).

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city --oidcAudience coraine

### `--oidcLeeway`

`-oidcLeeway` · `CORAINE_OIDCLEEWAY` · Int · default: `60`
· values: 0-600 (seconds) · plugin: oidc

The clock skew allowed between the broker and the provider, in seconds: a token is taken until `exp` plus
the leeway, and from `nbf` minus it. A token outside it is a 401 with
`error_description="expired, or not yet valid"`. A token found in the token cache has its `exp` and `nbf`
checked again with the same leeway on every request. Out of range: the broker does not start
(`value 601 for option '--oidcLeeway' exceeds its upper limit. Allowed range: 0-600`).

    coraine --auth oidc --oidcIssuer https://idp.example.org/realms/city --oidcLeeway 0

### `--oidcRefresh`

`-oidcRefresh` · `CORAINE_OIDCREFRESH` · Int · default: `300`
· values: 1-86400 (seconds) · plugin: oidc

Seconds between two fetches of the provider's keys (the discovery document, then its `jwks_uri`; or
`--oidcJwks`). While there are no keys the fetch is repeated every second, whatever the option says - the
provider may start after the broker. A token signed with a key the set does not have (the provider rotated
its keys) is a 401 and has the keys fetched at once, at most once a second. A new key set empties the token
cache.

    coraine --auth oidc --oidcIssuer https://idp.example.org/realms/city --oidcRefresh 60

### `--oidcRolesClaim`

`-oidcRolesClaim` · `CORAINE_OIDCROLESCLAIM` · String · default: `realm_access.roles`
· values: a claim's path, dot-separated · plugin: oidc

Where in the token the roles are: a path of claim names separated by dots, each one an object member. The
claim is an array of strings, or a string of space-separated values (`scope`). The default is Keycloak's
realm roles; `resource_access.<client>.roles` is a Keycloak client's roles, `scope`, `roles` or `groups`
other providers'. A token whose claim does not hold `--oidcReadRole` or `--oidcWriteRole` may do nothing:
403 (`the token has neither role '...' nor '...' (claim '...')`). An empty value stops the broker at start
(`--oidcRolesClaim, --oidcReadRole, --oidcWriteRole, --oidcTenantsClaim: none may be empty`).

    coraine --auth oidc --oidcIssuer https://idp.example.org --oidcRolesClaim scope

### `--oidcReadRole`

`-oidcReadRole` · `CORAINE_OIDCREADROLE` · String · default: `ngsild-read`
· values: a role name · plugin: oidc

The role that reads: `GET`, `HEAD`, `OPTIONS`, and the queries that are POSTs -
`POST /ngsi-ld/v1/entityOperations/query`, `POST /ngsi-ld/v1/temporal/entityOperations/query` and
`POST /ngsi-ld/v1/temporal/timepoint/entityOperations/query`. A token with only this role that writes gets a
403 (`a write needs the role 'ngsild-write' (claim 'realm_access.roles')`). Not empty (see
`--oidcRolesClaim`).

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city --oidcReadRole city-viewer

### `--oidcWriteRole`

`-oidcWriteRole` · `CORAINE_OIDCWRITEROLE` · String · default: `ngsild-write`
· values: a role name · plugin: oidc

The role that reads and writes: every request. Creating or updating a subscription is a write. Not empty
(see `--oidcRolesClaim`).

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city --oidcWriteRole city-editor

### `--oidcTenantsClaim`

`-oidcTenantsClaim` · `CORAINE_OIDCTENANTSCLAIM` · String · default: `tenants`
· values: a claim's path, dot-separated · plugin: oidc

Where in the token the tenants it may use are: a dot-separated path, as `--oidcRolesClaim`; an array of
strings, or a string of space-separated values. `default` is the default tenant (a request without
`NGSILD-Tenant`), `*` every tenant. A token without the claim may use the default tenant only; a token whose
claim names tenants may use those and no other - the default tenant only when `default` is among them. A
request in another tenant is a 403 (`the token may not use the tenant 't1' (claim 'tenants')`), decided
before the tenant is resolved, so a create in a tenant the token may not use makes no tenant. Not empty (see
`--oidcRolesClaim`). Not read with `--oidcAnyTenant`.

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city --oidcTenantsClaim org.tenants

### `--oidcAnyTenant`

`-oidcAnyTenant` · `CORAINE_OIDCANYTENANT` · Bool · default: `false`
· values: a flag · plugin: oidc

No tenant check: every token may use every tenant, and `--oidcTenantsClaim` is not read. The roles are
checked as without it. `GET /.well-known/oauth-protected-resource` then gives `"coraine_tenants": null`.

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city --oidcAnyTenant

### `--oidcAnonymousRead`

`-oidcAnonymousRead` · `CORAINE_OIDCANONYMOUSREAD` · Bool · default: `false`
· values: a flag · plugin: oidc

A read without a token (the reads of `--oidcReadRole`) is let in, in every tenant. A write without a token
is still a 401 (`no access token (Authorization: Bearer)`). A request that does send a token is decided on
the token, as without the option: a token that does not hold is a 401 even for a read.

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city --oidcAnonymousRead

### `--oidcOwnerMaxAge`

`-oidcOwnerMaxAge` · `CORAINE_OIDCOWNERMAXAGE` · Int · default: `86400` (a day)
· values: 1-31536000 (seconds) · plugin: oidc

A subscription's owner - the roles and tenants of the token that created it or last updated it - is kept
with the subscription and holds for this many seconds from that request. Each notification is sent only
while the owner holds, has a role that reads, and may use the subscription's tenant; after that none are
sent, until a request with a valid token updates the subscription and makes its sender the owner. This is
what bounds how long roles or tenants taken away at the provider still receive notifications: the token's
own `exp` is not used (a token lives minutes, a subscription months).

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city --oidcOwnerMaxAge 3600

## Plugin: fdsc - loaded by `--auth fdsc`

`fdsc.so` takes what the FIWARE Data Space Connector's provider side has, unchanged: the access token
VCVerifier issues for a Verifiable Presentation (a JWT, checked as APISIX's `openid-connect` checks it), and
the ODRL policies of the ODRL-PAP, evaluated in the broker as OPA evaluates the Rego the PAP generates. No
token, or one that does not hold: 401; no policy that allows the request: 403
(`no policy allows the request`). Keys and policies are fetched by a thread of the plugin and replaced
whole; until the keys are there a request with a token is a 503, and until the policies are there every
request is. A source of keys (`--fdscDiscovery` or `--fdscJwks`) and a
source of policies (`--fdscPap` or `--fdscPolicies`) are required. The broker can also hold the policies
itself, on a port of its own - `--policyApiPort`, a core option
([Authorization](authorization.md#7-the-policy-api---the-broker-as-its-own-pap)). Also documented in:
[Authorization](authorization.md#31-options).

### `--fdscDiscovery`

`-fdscDiscovery` · `CORAINE_FDSCDISCOVERY` · String · default: no default - `--fdscJwks` is needed instead
· values: a URL (`http://`, `https://`), or a file (a path, or `file://`) · plugin: fdsc

The verifier's OpenID discovery document - the URL APISIX's `openid-connect` has as `discovery`
(`http://verifier:3000/services/<service>/.well-known/openid-configuration`). Its `jwks_uri` gives the keys,
its `issuer` the `iss` a token must have unless `--fdscIssuer` is given. A document without an `issuer`, and
no `--fdscIssuer`: every request with a token is a 503
(`the verifier's issuer is not known (its discovery document has none, and --fdscIssuer is not set)`).
Without it and without `--fdscJwks` the broker stops at start:
`auth plugin 'fdsc': no keys to check a token with: --fdscDiscovery or --fdscJwks`. With both,
`--fdscJwks` is used and the discovery document is not read.

    coraine --auth fdsc --fdscDiscovery http://verifier:3000/services/data-service/.well-known/openid-configuration \
            --fdscPap http://odrl-pap:8080

### `--fdscJwks`

`-fdscJwks` · `CORAINE_FDSCJWKS` · String · default: no default - the keys come from `--fdscDiscovery`
· values: a URL (`http://`, `https://`), or a file (a path, or `file://`) · plugin: fdsc

The verifier's JWK Set itself, instead of the discovery document. It needs `--fdscIssuer`: the broker stops
at start with `auth plugin 'fdsc': --fdscJwks needs --fdscIssuer: a token's iss is always checked`. A JWK
Set that cannot be read is in the log, and requests with a token are a 503 until it can
(`the verifier's keys are not loaded yet`).

    coraine --database corDB --auth fdsc --fdscJwks /etc/coraine/jwks.json --fdscIssuer did:web:verifier.example.org \
            --fdscPolicies /etc/coraine/policies.json

### `--fdscIssuer`

`-fdscIssuer` · `CORAINE_FDSCISSUER` · String · default: no default - the discovery document's `issuer`
· values: the verifier's issuer (a URL or a DID) · plugin: fdsc

The `iss` every token must have. Without it, the one the discovery document gives (re-read at every
refresh). Required with `--fdscJwks`. A token of another issuer is a 401 with
`error_description="wrong issuer"`. It is what `GET /.well-known/oauth-protected-resource` gives as
`authorization_servers`.

    coraine --auth fdsc --fdscJwks /etc/coraine/jwks.json --fdscIssuer did:web:verifier.example.org --fdscPap http://odrl-pap:8080

### `--fdscAudience`

`-fdscAudience` · `CORAINE_FDSCAUDIENCE` · String · default: no default - `aud` is not checked
· values: an audience - the verifier's service id · plugin: fdsc

An audience the token's `aud` must hold. VCVerifier sets `aud` to the service id. Without it a token the
verifier issued for another service is accepted - as APISIX accepts it; with it, such a token is a 401 with
`error_description="wrong audience"`. When given, `GET /.well-known/oauth-protected-resource` has it as
`fdsc_audience`.

    coraine --auth fdsc --fdscDiscovery http://verifier:3000/services/data-service/.well-known/openid-configuration \
            --fdscAudience data-service --fdscPap http://odrl-pap:8080

### `--fdscLeeway`

`-fdscLeeway` · `CORAINE_FDSCLEEWAY` · Int · default: `120`
· values: 0-600 (seconds) · plugin: fdsc

The clock skew allowed on a token's `exp` and `nbf`, in seconds: a token is taken until `exp` plus the
leeway, and from `nbf` minus it. 120 is what APISIX's `openid-connect` (lua-resty-openidc) allows. A token
outside it is a 401 with `error_description="expired, or not yet valid"`; a token from the token cache is
checked again on every request.

    coraine --auth fdsc --fdscDiscovery http://verifier:3000/services/data-service/.well-known/openid-configuration \
            --fdscPap http://odrl-pap:8080 --fdscLeeway 30

### `--fdscPap`

`-fdscPap` · `CORAINE_FDSCPAP` · String · default: no default - `--fdscPolicies` is needed instead
· values: the ODRL-PAP's base URL · plugin: fdsc

The ODRL-PAP whose policies decide (`http://odrl-pap:8080`). They are read from its `GET /policy`, page by
page (`?page=<n>&pageSize=100`) until a page is empty, and installed only when every page has been read: a
fetch that fails half-way keeps the policies in use. A policy that cannot be read is skipped and said in the
log. Without it and without `--fdscPolicies` the broker stops at start:
`auth plugin 'fdsc': no policies to decide with: --fdscPap or --fdscPolicies`. With both, `--fdscPolicies`
is used.

The broker's own policy API (`--policyApiPort`) is such a PAP: `--fdscPap http://127.0.0.1:<port>`.

    coraine --database corDB --policyApiPort 8081 --auth fdsc --fdscPap http://127.0.0.1:8081 \
            --fdscJwks /etc/coraine/jwks.json --fdscIssuer did:web:verifier.example.org

### `--fdscPolicies`

`-fdscPolicies` · `CORAINE_FDSCPOLICIES` · String · default: no default - `--fdscPap` is needed instead
· values: a URL (`http://`, `https://`), or a file (a path, or `file://`) · plugin: fdsc

The ODRL policies themselves, instead of a PAP: a JSON array - of ODRL policies, or the PAP's own listing
(`[{"id", "odrl:uid", "odrl": "<the policy as a string>"}]`) - a single policy object, or an object whose `@graph`
holds the policies. Read again every
`--fdscRefresh` seconds. A file that is not JSON leaves the policies in use (none at first: every request
is a 503, `the policies are not loaded yet`).

    coraine --database corDB --auth fdsc --fdscJwks /etc/coraine/jwks.json --fdscIssuer did:web:verifier.example.org \
            --fdscPolicies /etc/coraine/policies.json

### `--fdscOrganization`

`-fdscOrganization` · `CORAINE_FDSCORGANIZATION` · String · default: no default - the PAP's default DID, `urn:ngsi-ld:organization:0b03975e-7ded-4fbd-9c3b-a5d6550df7e2`
· values: a DID · plugin: fdsc

The provider's DID - the PAP's `GENERAL_ORGANIZATION_DID`. A policy's `vc:role` reads the roles the
credential's subject has for this organisation: the `names` of the entry of `credentialSubject.roles` whose
`target` is this DID. A credential whose roles are for another target holds no role here. Two entries for
the organisation with different names are OPA's `eval_conflict_error`: a 503. Set it to what the PAP has.

    coraine --auth fdsc --fdscDiscovery http://verifier:3000/services/data-service/.well-known/openid-configuration \
            --fdscPap http://odrl-pap:8080 --fdscOrganization did:web:provider.example.org

### `--fdscRefresh`

`-fdscRefresh` · `CORAINE_FDSCREFRESH` · Int · default: `30`
· values: 1-86400 (seconds) · plugin: fdsc

Seconds between two fetches of the keys and the policies. While either is missing, both are fetched every
second, whatever the option says - the verifier and the PAP may start after the broker. A token signed with
a key the set does not have (VCVerifier makes a new key at every start) is a 401 and has the keys fetched at
once, at most once a second. A policy written to the PAP is in use within this many seconds. A new key set
empties the token cache.

    coraine --auth fdsc --fdscDiscovery http://verifier:3000/services/data-service/.well-known/openid-configuration \
            --fdscPap http://odrl-pap:8080 --fdscRefresh 5

### `--fdscAnonymous`

`-fdscAnonymous` · `CORAINE_FDSCANONYMOUS` · Bool · default: `false`
· values: a flag · plugin: fdsc

A request without a token is not a 401: it is let in as no one, and the policies decide it - only a policy
whose assignee is `vc:any` can allow it anything; for the rest it is a 403. A request with a token that does
not hold is still a 401.

    coraine --database corDB --auth fdsc --fdscJwks /etc/coraine/jwks.json --fdscIssuer did:web:verifier.example.org \
            --fdscPolicies /etc/coraine/policies.json --fdscAnonymous

### `--fdscNgsild`

`-fdscNgsild` · `CORAINE_FDSCNGSILD` · Bool · default: `false`
· values: a flag · plugin: fdsc

Decide on the entity, as NGSI-LD has it, instead of as the Data Space Connector does - so decisions differ
from the connector's. `ngsi-ld:entityType` is then the entity's own types, expanded: the body's for a
create, the stored entity's for everything else; each entity of a batch on its own (a denied one is its 403
in the 207), each entity of a query on its own (the denied ones left out); the action is the NGSI-LD
operation, not the HTTP method. Without it, `ngsi-ld:entityType` is what the connector reads: the type part
of the id in the path, else `?type=` as one string, else the body's `type`.

Only with it: the words `ngsi-ld:tenant` and `ngsi-ld:attribute` (a policy that uses one is skipped without
it, and the log says why:
`policy '<uid>' skipped: 'ngsi-ld:tenant' is coraine's own word - only with --fdscNgsild`); and the
notifications - decided for the subscription's owner, entity by entity, the owner's credential kept with the
subscription (`--fdscOwnerMaxAge`). Without it a subscription notifies what matches, and no credential is
kept. Not available in the APISIX plugin: it needs the stored entity
([Authorization](authorization.md#6---fdscngsild---decided-on-the-entity)).

    coraine --database corDB --auth fdsc --fdscJwks /etc/coraine/jwks.json --fdscIssuer did:web:verifier.example.org \
            --fdscPolicies /etc/coraine/policies.json --fdscNgsild

### `--fdscOwnerMaxAge`

`-fdscOwnerMaxAge` · `CORAINE_FDSCOWNERMAXAGE` · Int · default: `0` (until the credential's own end)
· values: 0-31536000 (seconds) · plugin: fdsc

With `--fdscNgsild` only (without it no owner is kept, and the option changes nothing). A subscription's
owner - the credential of the token that made or last updated it, kept for its notifications - holds for
this many seconds from that request; after that its notifications go without entities, until the owner
updates the subscription. 0: until the credential's own `validUntil` (VC 1.1: `expirationDate`); a
credential past it no longer holds whatever this option says. A kept credential is never checked again for
revocation: this is what bounds how long a revoked one still receives notifications.

    coraine --database corDB --auth fdsc --fdscJwks /etc/coraine/jwks.json --fdscIssuer did:web:verifier.example.org \
            --fdscPolicies /etc/coraine/policies.json --fdscNgsild --fdscOwnerMaxAge 86400

### `--fdscStrippedPath`

`-fdscStrippedPath` · `CORAINE_FDSCSTRIPPEDPATH` · Bool · default: `false`
· values: a flag · plugin: fdsc

`http:path` reads the `x-stripped-path` header when the request has one, as the connector's Rego does;
without the option it reads the request's own path, and the header is ignored. Only behind a gateway that
sets the header and drops a client's: a client that can send it chooses the path an `http:isInPath` rule
sees ([Authorization](authorization.md#44-where-coraine-decides-otherwise-on-purpose)).

    coraine --auth fdsc --fdscDiscovery http://verifier:3000/services/data-service/.well-known/openid-configuration \
            --fdscPap http://odrl-pap:8080 --fdscStrippedPath

---

## coraine-import

`coraine-import` is the writer half of a migration: a program of its own beside the broker, not a broker
option. It reads a migration stream - one JSON record per line, expanded NGSI-LD - writes it into the stores
its options name, prints a report on stderr (its log is stdout) and exits: 0 when every record was imported,
1 when any was refused or the stores could not be opened. It loads the broker's store plugins and takes
their options by the broker's names, so what it writes is what a broker started with the same store
options reads; the broker must not run on those stores during the import. The options below are
coraine-import's: `coraine-import -u` lists them, with the options of the store plugins it loads
(`coraine-import --database corDB --troe corDB -u` lists corDB's). Its environment variables have the
broker's `CORAINE_` prefix and the same names as the broker's - a shell that sets `CORAINE_DATABASE` for a
broker sets it for the import too. Built with `COR_FEATURE_MIGRATE` (on by default). Also documented in:
[Migrating a database to coraine](migration.md#step-by-step).

### `--file`

`-f` · `CORAINE_FILE` · String · default: no default - required (see below)
· values: a path, or `-` for stdin · plugin: coraine-import

The migration stream: one JSON object per line, records of kind `header`, `entity`, `subscription`,
`registration`, `temporalEntity` and `temporalInstance`, every name expanded
([The stream](migration.md#the-stream)). Records are written in the order they come. A refused record is
named on stderr by its line in the stream and the reason, and the import goes on with the next:

    stream.ndjson:4: entity: entity type 'Room' is not expanded - the stream must hold expanded NGSI-LD (full IRIs, core terms short)

A record already in the target store is refused - an import never overwrites - so importing the same stream
again refuses everything that went in the first time
(`-:2: entity: 'urn:ngsi-ld:Room:1' already exists in the target`). A file that cannot be opened:
`coraine-import: cannot open '<path>'`, exit code 1.

The option is marked required, but a missing `--file` is not refused at start: the stores are opened first,
and with `--database corDB` the program then crashes (SIGSEGV). Always give it.

    coraine-import --database corDB --dbDir /var/lib/coraine --troe corDB --file stream.ndjson
    coraine-import -db corDB --dbDir /var/lib/coraine -f - < stream.ndjson

### `--database`

`-db` · `CORAINE_DATABASE` · String · default: `mongoc`
· values: `mongoc`, `corDB`, `ramDB` (the plugins in `<plugin dir>/db/currentState/`), or a path to a `.so` · plugin: coraine-import

The current-state store the entities, subscriptions and registrations are written into - the broker's
`--database`, with the same plugins and the same resolution (`/opt/seamware/plugins`, or
`SEAMWARE_PLUGIN_DIR`). Its plugin's options are added to the command line: `--dbHost` ... for mongoc,
`--dbDir` ... for corDB ([Plugin: corDB](#plugin-cordb-loaded-by-database-cordb)). `ramDB` keeps
nothing after the import exits. A plugin that is not there stops the import with the usage
(`DB plugin 'bogus' (/opt/seamware/plugins/db/currentState/bogus.so): not found`, exit code 1); a store that
cannot be opened too (`corDB: --dbDir '/x': Permission denied` ... `main: dbStart failed`, exit code 1).

    coraine-import --database corDB --dbDir /var/lib/coraine --file stream.ndjson
    coraine-import --database mongoc --dbHost mongo --file stream.ndjson

### `--troe`

`-troe` · `CORAINE_TROE` · String · default: `none`
· values: `none`, `timescale`, `corDB`, `ramDB`, or a path to a `.so` · plugin: coraine-import

The temporal store the history (`temporalEntity` and `temporalInstance` records) is written into - the
broker's `--troe`, with the same plugins; its plugin's options are added (`--troeHost` ... for timescale).
The history is written in batches of 500 events, one transaction each. With `none` every history record is
refused (`temporalEntity: the importer runs without a TRoE store (--troe) - nowhere to put the history`),
and the exit code is 1. The current state is written as it is: a store that records history at its write
sites (`--troe corDB`) records none for the import's writes - the history is what the stream says.

    coraine-import --database corDB --dbDir /var/lib/coraine --troe corDB --file stream.ndjson
    coraine-import --database mongoc --dbHost mongo --troe timescale --troeHost pg --file stream.ndjson

### `--verbose`

`-v` · `CORAINE_VERBOSE` · Bool · default: `false`
· values: a flag · plugin: coraine-import

The info and verbose lines of the log (`I:` and `V:`, stdout): the store plugins coming up, the tenants
created, the stores closed. The report on stderr is printed with or without it.

    coraine-import -v --database corDB --dbDir /var/lib/coraine --file stream.ndjson

### `--debug`

`-d` · `CORAINE_DEBUG` · Bool · default: `false`
· values: a flag · plugin: coraine-import

The debug lines of the log (`D:`, stdout). Independent of `--verbose`.

    coraine-import -d --database corDB --dbDir /var/lib/coraine --file stream.ndjson

### `--traceLevels`

`-t` · `CORAINE_TRACELEVELS` · String · default: no default - no trace levels
· values: trace levels - numbers and ranges, comma-separated (`0-255`) · plugin: coraine-import

The trace levels of the log, as the broker's `--traceLevels`. A release build has its traces compiled out:
a trace level then turns nothing on, and the log says so once
(`--traceLevels ...: this broker is a release build, its traces are compiled out - nothing to turn on`).
A build with traces: `coraine-build --debug`.

    coraine-import -t 0-255 --database corDB --dbDir /var/lib/coraine --file stream.ndjson

### `--version`

`-V` · `CORAINE_VERSION` · Bool · default: `false`
· values: a flag · plugin: coraine-import

Prints `coraine-import <version>` and exits 0, before anything else is read or loaded.

    $ coraine-import -V
    coraine-import 0.6.0-dev

### `--usage`

`-u` · no environment variable · Bool · default: `false`
· values: a flag · plugin: coraine-import

Prints the options - those of the store plugins `--database` and `--troe` name included - and exits 0.
An unknown option prints the same after `coraine-import: unrecognized option: <option>`, and exits 1.

    coraine-import --database corDB --troe corDB -u

### `--Usage`

`-U` · no environment variable · Bool · default: `false`
· values: a flag · plugin: coraine-import

Prints the options as a table - long and short name, environment variable, required or optional, type, and
where the value came from - and exits 0.

    coraine-import -U

**The store plugins' options.** The options below are not coraine-import's: they are the mongoc plugin's, present because `--database`
defaults to `mongoc`, and they are the broker's options of the same names (owner: the mongoc plugin -
[Plugin: mongoc](#plugin-mongoc-loaded-by-database-mongoc-the-default)). With `--database corDB`
they are not there, and corDB's are; with `--troe timescale`, timescale's are added
([Plugin: timescale](#plugin-timescale-loaded-by-troe-timescale)). An import into mongoc was not run here
(it needs MongoDB).

### `--dbHost`

`-dbHost` · `CORAINE_DBHOST` · String · default: `localhost`
· values: a host name or address · plugin: mongoc

The MongoDB server's host, as the broker's `--dbHost`. Not used with `--dbURI`.

    coraine-import --database mongoc --dbHost mongo --file stream.ndjson

### `--dbPort`

`-dbPort` · `CORAINE_DBPORT` · UShort · default: `27017`
· values: 1-65535 · plugin: mongoc

The MongoDB server's port, as the broker's `--dbPort`.

    coraine-import --database mongoc --dbHost mongo --dbPort 27018 --file stream.ndjson

### `--dbName`

`-dbName` · `CORAINE_DBNAME` · String · default: `cor`
· values: a database name prefix · plugin: mongoc

The database name of the default tenant, and the prefix of every tenant's (`cor-t1`), as the broker's
`--dbName` - the import and the broker must use the same.

    coraine-import --database mongoc --dbHost mongo --dbName city --file stream.ndjson

### `--globalDb`

`-globalDb` · `CORAINE_GLOBALDB` · String · default: `coraine`
· values: a database name · plugin: mongoc

The database for the state of no tenant (the hosted JSON-LD contexts), as the broker's `--globalDb`.

    coraine-import --database mongoc --dbHost mongo --globalDb coraine --file stream.ndjson

### `--dbUser`

`-dbUser` · `CORAINE_DBUSER` · String · default: no default - no authentication
· values: a MongoDB user · plugin: mongoc

The MongoDB user, as the broker's `--dbUser` - used only when `--dbPwd` is given too, and not with `--dbURI`; percent-encoded, as there.

    coraine-import --database mongoc --dbHost mongo --dbUser coraine --dbPwd "$PW" --file stream.ndjson

### `--dbPwd`

`-dbPwd` · `CORAINE_DBPWD` · String · default: no default - no password
· values: a password · plugin: mongoc

The MongoDB user's password, as the broker's `--dbPwd` - used only with `--dbUser`, and not with `--dbURI`; percent-encoded, as there. `CORAINE_DBPWD` keeps it off the command line.

    CORAINE_DBPWD=... coraine-import --database mongoc --dbHost mongo --dbUser coraine --file stream.ndjson

### `--dbURI`

`-dbURI` · `CORAINE_DBURI` · String · default: no default - built from `--dbHost`, `--dbPort`, `--dbUser`, `--dbPwd`
· values: a MongoDB connection URI · plugin: mongoc

The whole connection URI (a replica set, TLS, options), as the broker's `--dbURI`: `--dbHost`, `--dbPort`, `--dbUser` and `--dbPwd` are then not used.

    coraine-import --database mongoc --dbURI 'mongodb://mongo-0,mongo-1/?replicaSet=rs0' --file stream.ndjson

### `--dbTimeout`

`-dbTimeout` · `CORAINE_DBTIMEOUT` · UShort · default: `30`
· values: 1-3600 (seconds) · plugin: mongoc

How long the connection to MongoDB is tried before the import gives up (`mongoc: ping failed: No suitable
servers found` ... `main: dbStart failed`, exit code 1), as the broker's `--dbTimeout`.

    coraine-import --database mongoc --dbHost mongo --dbTimeout 5 --file stream.ndjson

### `--dbValueIndexes`

`-dbValueIndexes` · `CORAINE_DBVALUEINDEXES` · String · default: no default - no declared indexes
· values: expanded attribute names, comma-separated, or a JSON array of `{attribute, type, tenant}` · plugin: mongoc

The declared indexes - the attributes whose `q` an index serves - as the broker's `--dbValueIndexes`.

    coraine-import --database mongoc --dbHost mongo --dbValueIndexes https://ex.org/temperature --file stream.ndjson

## coraine-build

`coraine-build` builds a coraine of your own - feature switches, HTTP server, bridges, profile-guided or
tuned - from the exact source of the installed packages (the `coraine-dev` package's tarball), or from a
directory of git clones. A shell script beside the broker (`packaging/deb/coraine-build` in the source), not
a broker option. Everything is written into the directory it is given; root is not needed. The result is in
`<dir>/install`: `bin/coraine` (or `--name`'s), `bin/coraine-import`, `plugins/...`,
`etc/contextSourceExtras.json`, with `--tune` `etc/tune.env`, with dds `etc/bridges.json` and
`etc/dds-types/`, and `env` - `. <dir>/install/env` puts `bin/` first on `PATH` and points the broker at
its own plugins and etc (`SEAMWARE_PLUGIN_DIR`, `SEAMWARE_ETC_DIR`, `CORAINE_CONTEXTSOURCEEXTRAS`). The
options below are coraine-build's; it reads no `CORAINE_*` variables. `<dir>`, its one argument, is a new
or empty directory, or one an earlier coraine-build unpacked into (the source is then reused:
`coraine-build: reusing the source already in <dir>`); any other directory is refused
(`<dir> is neither empty nor a coraine-build directory`). `<dir>/install` is emptied at every build. Without
`<dir>` (and without a list option) it prints the help and exits 1. An unknown option:
`coraine-build: unknown option '<option>' (--help)`, exit 1. Each phase is one line
(`coraine-build: phase build`, `install`, `bridges`, `check`, `done`; `tools`, `train` with `--pgo` /
`--tune`). Also documented in: [Building from source - in detail](building-details.md#every-choice-coraine-build-offers).

### `--release`

no short form · no environment variable · flag · default: the build mode when no other is given
· values: a flag · tool: coraine-build

Optimised, with the traces compiled out - what the coraine package holds before profile-guiding.
`--release`, `--debug`, `--pgo` and `--tune` are one choice: the last one given decides.

    coraine-build --source ~/git --bridges none /tmp/cb

### `--debug`

no short form · no environment variable · flag · default: off
· values: a flag · tool: coraine-build

The traces compiled in, so `--traceLevels` turns them on - for reproducing a crash report. The bridges are
built in their debug flavour too.

    coraine-build --debug --source ~/git /tmp/cb

### `--pgo`

no short form · no environment variable · flag · default: off
· values: a flag · tool: coraine-build

Profile-guided release: built instrumented, trained here on the measured request shapes, rebuilt with the
profile - what the coraine package and the image hold. A few minutes more than `--release`; needs `curl`,
`wrk` and `python3` (`coraine-build: --pgo needs wrk`). The training's tools (corTestClient, corRequest) are
built into `<dir>` first, and its output is kept in `<dir>/pgo.log`; a part of the training that fails
stops the build (the profile would lack it). Not with `--http-server builtin`
([Performance](performance.md)).

    coraine-build --pgo --source ~/git /tmp/cb

### `--tune`

no short form · no environment variable · String · default: no default - not tuned
· values: a workload file · tool: coraine-build

`--pgo` trained on your workload instead of the measured request shapes, and the knobs measured on it - the
best kept in `<dir>/install/etc/tune.env` (`TUNE_DBLOCKPREFER`, `TUNE_ALLOCATOR`: the `--dbLockPrefer` and
the allocator to preload). Needs what `--pgo` needs; a file that is not there:
`coraine-build: --tune: no workload file '<file>'`. Not with `--http-server builtin`
([Extreme performance](extreme-performance.md#the-workload-file)).

    coraine-build --tune workload.json --source ~/git /tmp/cb

### `--features`

no short form · no environment variable · String · default: no default - every switch at its default
(`--list-features`)
· values: comma-separated `NAME=ON|OFF` (also `on`/`off`, `1`/`0`), `NAME` with or without `COR_FEATURE_`; `--features=<list>` too · tool: coraine-build

The feature switches: what is compiled into the broker and its plugins, and what is left out - a route left
out answers 501 (`'GET /ngsi-ld/v1/csourceRegistrations' is not included in this build of coraine`). A name
that is not a switch: `coraine-build: --features: no feature 'FOO' (--list-features)`; a value that is not
ON or OFF is refused too. `MONGOC=OFF` builds without the mongo-c driver, which is otherwise needed (v2 -
`pkg-config mongoc2`, or the one `coraine-dev` bundles in `/opt/seamware`); `ICU_COLLATION=ON` needs ICU
(`libicu-dev`). Switches that need another one are turned off without it (`--list-features --json` names
them, `requires`) ([Compiling out what you don't need](building-details.md#compiling-out-what-you-dont-need)).

    coraine-build --features REGISTRATIONS=OFF,SERVICE_EXECUTION=OFF,MONGOC=OFF --source ~/git /tmp/cb

### `--http-server`

no short form · no environment variable · String · default: `mhd`
· values: `mhd` (libmicrohttpd), `builtin` (corHttp, no external library); `--http-server=<s>` too · tool: coraine-build

The broker's HTTP server. With `builtin` the switches that need libmicrohttpd are off - `POLICY_API`, so the
broker built has no `--policyApiPort`. Not with `--pgo` or `--tune`
(`coraine-build: --http-server builtin: not with --pgo yet (its training tools are built and tried with
libmicrohttpd only)`). Another value: `coraine-build: --http-server: '<s>' - mhd or builtin`
([Choosing the HTTP server](building-details.md#choosing-the-http-server)).

    coraine-build --http-server builtin --source ~/git /tmp/cb

### `--bridges`

no short form · no environment variable · String · default: `mqtt,modbus`
· values: comma-separated `mqtt`, `modbus`, `dds`, `ckan`, `opcua`, or `none`; `--bridges=<list>` too · tool: coraine-build

The bridge plugins to build beside the broker, each from its own repository in the source, into
`<dir>/install/plugins/bridge/` (the broker's own `loopback.so` is always built). A name that is not a bridge
stops the build (`coraine-build: --bridges: no bridge '<b>' (--list-bridges)`), and so does one that
cannot be built here, saying why (`--bridges: mqtt cannot be built here - libmosquitto's headers are not
installed (libmosquitto-dev)`) - before anything is compiled. With `dds`, `etc/bridges.json` is the image's
default DDS configuration (domain 0, no topics) with `etc/dds-types/`: the DDS bridge does not start without
one.

    coraine-build --bridges mqtt,dds --source ~/git /tmp/cb
    coraine-build --bridges none --source ~/git /tmp/cb

### `--list-bridges`

no short form · no environment variable · flag · default: off
· values: a flag · tool: coraine-build

Prints every bridge - its name, whether it is a default, what it is, and, when it cannot be built here, why
not - and exits 0. With `--json`: a JSON array of `{name, default, description, available, why}`. No
`<dir>` is needed (a temporary one is used and removed); with `--source <dir of clones>` the clones are read
as they are checked out, nothing is exported.

    $ coraine-build --list-bridges --source ~/git
    coraine 0.6.0~git20261023.cf852586 - bridges (--bridges NAME,...):
      mqtt     default  MQTT - NGSI-LD over MQTT topics (mqtt.so, libmosquitto)
      modbus   default  Modbus TCP - values between Modbus registers and entities (modbus.so, no library)
      dds               DDS - topics, services and actions between DDS and entities (dds.so, eProsima Fast DDS and the DDS Enabler)
      ...

### `--list-features`

no short form · no environment variable · flag · default: off
· values: a flag · tool: coraine-build

Prints every feature switch of the source - its name (without `COR_FEATURE_`), its default and what it
is - and exits 0. With `--json`: a JSON array of `{name, default, description, group, requires}`. No `<dir>`
is needed, as for `--list-bridges`.

    $ coraine-build --list-features --source ~/git
    coraine 0.6.0~git20261023.cf852586 - feature switches (--features NAME=ON|OFF):
      SUBSCRIPTIONS            ON   Enable Context Information Subscriptions + notifications
      REGISTRATIONS            ON   Enable Context Source Registration CRUD (§ 12)
      ...

### `--json`

no short form · no environment variable · flag · default: off
· values: a flag · tool: coraine-build

With `--list-features` or `--list-bridges`: the list as a JSON array. For a feature, `group` is the
CMakeLists.txt group it is in (`# Group: <name> - ...`), and `requires` names the switches it is turned off
without, and a build setting as `NAME=value` (`"HTTP_SERVER=mhd"`: libmicrohttpd, not the built-in server).
Without a list option it changes nothing.

    $ coraine-build --list-features --json --source ~/git
    ...
      {"name": "POLICY_API", "default": true, "description": "...", "group": "Security", "requires": ["HTTP_SERVER=mhd"]},

### `--name`

no short form · no environment variable · String · default: `coraine`
· values: 1-64 of `A-Z a-z 0-9 . _ -`, starting with a letter or a digit; not `coraine-import`; `--name=<exe>` too · tool: coraine-build

The broker's executable: `<dir>/install/bin/<exe>` - a broker built for one role (a cor-agent, an edge
node) named after it. It reads the same `CORAINE_*` environment variables whatever it is called. Refused:
`coraine-build: --name coraine-import is the migration tool's`, and a name of other characters.

    coraine-build --name edge1 --bridges none --source ~/git /tmp/cb
    . /tmp/cb/install/env && edge1 --database corDB

### `--source`

no short form · `LIBS_REF` (with a directory) · String · default: `/usr/share/coraine/src/coraine-src.tar.xz` (installed by `coraine-dev`)
· values: a source tarball, or a directory of git clones · tool: coraine-build

Where the source comes from.

- A file: the source tarball. Without `--source` and without `coraine-dev` installed, nothing can be built
  or listed: `coraine-build: no source tarball at /usr/share/coraine/src/coraine-src.tar.xz (is coraine-dev
  installed? --source names another, or a directory of clones)`.
- A directory: git clones of coraine and every Cor-Lib side by side, as `corLibs/bootstrap.sh` clones them
  (`~/git`). The tarball is exported from them as the packages' is (`coraine/packaging/deb/source.sh`):
  coraine at the commit checked out, each lib at `origin/main` - or, with `LIBS_REF=HEAD` in the
  environment, at the branch each clone has checked out. Committed trees only: uncommitted changes are not
  built. `coraine-dev` is not needed. A directory without a coraine clone:
  `coraine-build: --source <dir>: no coraine clone in it (coraine/packaging/deb/source.sh)`.

The source is unpacked into `<dir>` once; a later build in the same `<dir>` reuses it, whatever `--source`
then says.

    coraine-build --source ~/git /tmp/cb
    LIBS_REF=HEAD coraine-build --source ~/git /tmp/cb

### `--help`

`-h` · no environment variable · flag · default: off
· values: a flag · tool: coraine-build

Prints the help and exits 0.

    coraine-build --help
