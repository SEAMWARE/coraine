# Installation & Administration Guide

## What coraine needs

coraine is a single binary plus the plugins it loads at startup. A working broker
with the in-memory store needs **no external service at all**. The MongoDB backend
needs a MongoDB server; the TimescaleDB temporal backend needs PostgreSQL with the
TimescaleDB extension.

### MongoDB server

**MongoDB 8.2** (`mongo:8.2`): the functional suite, CI and the performance figures run on it.

MongoDB 8.0 does not start on a Linux kernel 6.19 or newer - mongod exits at once with
`MongoDB cannot start: Linux kernel versions 6.19 and newer has a known incompatibility with this
version of MongoDB` ([SERVER-121912](https://jira.mongodb.org/browse/SERVER-121912)). In a container
the kernel is the host's, so `mongo:8.0` fails the same way on such a host. 8.2 starts on them.

`--high-availability mongo` needs a replica set ([High Availability](high-availability.md)).

### System packages (Debian / Ubuntu)

| Need | Package |
|------|---------|
| HTTP server | `libmicrohttpd-dev` |
| TLS | `libssl-dev` |
| MQTT bridge plugin (`mqtt.so`, notifications to `mqtt://`) | `libmosquitto-dev` |
| Geo queries | `libgeos-dev` |
| `orderBy` collation | `libicu-dev` |
| MongoDB driver (`mongoc` plugin) | mongo-c **v2** (`mongoc2.pc` via pkg-config) |
| TimescaleDB plugin | `libpq-dev` |
| Toolchain | `cmake build-essential` |

The mongo-c **v2** driver is the most common build snag — it is packaged by few
distributions and is normally compiled from source. Build without MongoDB support
with `cmake -DCOR_FEATURE_MONGOC=OFF` and run with `--database corDB`.

## Install from source

The dependency stack is a set of sibling repositories. The `corLibs` umbrella clones
and builds all of them at their pinned versions:

```sh
git clone git@github.com:SEAMWARE/corLibs.git
./corLibs/bootstrap.sh
cd coraine
make i          # release build + install
```

`make install` writes:

- the broker to `/usr/local/bin/coraine`
- the plugins to `/opt/seamware/plugins/{db/currentState,troe/temporal,api}/`
- the provenance file to `/opt/seamware/etc/contextSourceExtras.json`

Run with sufficient privileges, or pre-create those directories.

[Building from source](building.md) has every system package and all the make
targets; [its detail page](building-details.md) the source layout and how to
compile features out. This page covers the common case only.

## Install with apt

Two Debian packages, for **Ubuntu 26.04**, **Ubuntu 24.04 LTS** and **Debian 13 (trixie)**, on
**amd64** and **arm64**:

| Package | What it holds |
|---------|---------------|
| `coraine` | the broker (`/usr/bin/coraine`), `coraine-import`, every plugin but the DDS bridge (`/opt/seamware/plugins`), `/opt/seamware/etc/contextSourceExtras.json`, the systemd unit `coraine.service` and its options file `/etc/default/coraine`, and [c³](https://github.com/SEAMWARE/c3), the control center (`/usr/bin/c3`, its pages in `/usr/share/c3/www`): Operate, Explore and Documentation; Compile with `coraine-dev` |
| `coraine-dev` | `coraine-build` (`/usr/bin`), the exact source of the same version - coraine and every Cor-Lib at the commits built - in `/usr/share/coraine/src/coraine-src.tar.xz` and its `MANIFEST.txt` (`/usr/share/doc/coraine-dev`), and the toolchain and libraries to build it (as dependencies). No installed headers and no libraries of coraine's ([coraine-dev](#coraine-dev-build-your-own)) |

From the signed apt repository, **https://seamware.github.io/apt**:

```sh
curl -fsSL https://seamware.github.io/apt/coraine.gpg | sudo tee /usr/share/keyrings/coraine.gpg > /dev/null
echo "deb [signed-by=/usr/share/keyrings/coraine.gpg] https://seamware.github.io/apt $(. /etc/os-release; echo $VERSION_CODENAME) main" \
  | sudo tee /etc/apt/sources.list.d/coraine.list
sudo apt update && sudo apt install coraine        # or coraine-dev
```

The component `main` holds the releases; `testing` holds builds of a commit between releases (put
`testing` in place of `main` on that line). The packages are built and published by the GitHub
workflow `Packages` (`.github/workflows/packages.yml`) - for every release, and on demand - which also
keeps each run's `.deb` files as artifacts (`debs-<dist>-<arch>`), for an installation without the
repository:

```sh
sudo apt install ./coraine_0.6.1+ubuntu24.04_amd64.deb      # apt resolves the dependencies
```

Versions, `<dist>` being `ubuntu26.04`, `ubuntu24.04` or `debian13`:

| Version | What |
|---------|------|
| `0.6.1+<dist>` | the release 0.6.1 |
| `0.6.1+<dist>.<N>` | a package revision: the release 0.6.1 built again with a fix of c³'s, the broker the same (`MANIFEST.txt` names c³'s commit) |
| `0.6.1+<dist>.<YYYYMMDD>.git<sha8>` | a build of a later commit of `main` (component `testing`) |

Each sorts after the one above it and before the next release (`0.6.2+<dist>`); builds of commits sort
by date. `apt upgrade` installs a package revision over its release, and a build of a commit over both.

### The coraine package

- **Dependencies** come from the distribution: OpenSSL, GEOS, libmicrohttpd, libmosquitto, libpq, zstd -
  and, on Ubuntu 26.04, the MongoDB C driver v2 (`libmongoc2-2`). Ubuntu 24.04 and Debian 13 package
  only v1 of that driver, so there the package carries v2 itself, privately, in `/opt/seamware/lib`: the
  `mongoc` plugin finds it through its RUNPATH, and the system's `libmongoc-1.0` is not touched.
- **The service** is installed but **not enabled and not started**: the broker listens on every
  interface and has no authentication of its own. Start it when it is configured:

  ```sh
  sudoedit /etc/default/coraine
  sudo systemctl enable --now coraine
  journalctl -u coraine -f
  ```

  It runs `coraine` in the foreground (`Type=simple`, the log on stdout - in the journal) as the system
  user `coraine`, with the options in `/etc/default/coraine`
  ([Environment variables](#environment-variables)). Out of the box: `corDB` on disk in
  `/var/lib/coraine/db`, no temporal history, port 1026. `systemctl stop` is a clean stop (SIGTERM -
  corDB syncs its log and snapshots, [corDB on disk](#cordb-on-disk)). An upgrade restarts a running
  broker.
- **Removing** the package stops the broker; **purging** it removes `/etc/default/coraine`, but keeps the
  data in `/var/lib/coraine` and the user `coraine`.

### coraine-dev: build your own

```sh
coraine-build --list-features
coraine-build --features REGISTRATIONS=OFF,SERVICE_EXECUTION=OFF ~/coraine-small
. ~/coraine-small/install/env
coraine --database corDB
```

What the package installs, and nothing else:

| Path | What |
|------|------|
| `/usr/bin/coraine-build` | the build script (`coraine-build --help`) |
| `/usr/share/coraine/src/coraine-src.tar.xz` | the source the `coraine` package of the same version was built from |
| `/usr/share/doc/coraine-dev/MANIFEST.txt` | every repository in the tarball and the commit it was taken at |
| `/opt/seamware/include`, `/opt/seamware/lib/{libmongoc2.so,libbson2.so,pkgconfig,cmake}` | Ubuntu 24.04 and Debian 13 only: the development files of the MongoDB C driver v2 the `coraine` package bundles |

It installs **no coraine or Cor-Lib headers and no libraries** (no `/usr/include/cor*`, no `libcor*.a`).
The headers are in the tarball. Its layout is the one the build expects - coraine and the Cor-Libs as
sibling directories:

```
coraine-<version>/
├── SOURCE          version, gitSha, commitDate
├── MANIFEST.txt
├── coraine/
├── corBase/  corLog/  corAlloc/  corArgs/  corHash/  corTree/  corJson/  corProm/
├── corPlugin/  corBridge/  corHttp/  corRest/  corJsonld/  corNgsild/  corDB/
├── corDdsBridge/  corModbusBridge/  corMqttBridge/  corCkanBridge/  corOpcuaBridge/
├── corTools/  corTest/  corLibs/
└── c3/
```

`coraine-build [options] <dir>` unpacks it into `<dir>` (without the `coraine-<version>/` level) and builds
there, as any user; nothing is written outside `<dir>`. The result is in `<dir>/install` (`bin/`,
`plugins/`, `etc/`, and `env`, which points `PATH`, `SEAMWARE_PLUGIN_DIR`, `SEAMWARE_ETC_DIR` and
`CORAINE_CONTEXTSOURCEEXTRAS` at it). Given a `<dir>` it has unpacked before, it reuses that source.

**From git clones, without `coraine-dev`.** `--source <clones>` takes a directory of clones instead of
the tarball - coraine and every Cor-Lib side by side, as `corLibs/bootstrap.sh` clones them. The
tarball is made from them as the packages' is (`<clones>/coraine/packaging/deb/source.sh`): coraine at
the commit checked out, each lib at `origin/main` (`LIBS_REF=HEAD`: the branch each clone has checked
out). Committed trees only - an uncommitted change is not built. Run the coraine clone's own script:

```sh
~/git/coraine/packaging/deb/coraine-build --source ~/git --pgo ~/coraine-pgo
```

c³ built from such clones does the same when `coraine-dev` is not installed.

With the DDS bridge built (`--bridges dds`), `etc/bridges.json` holds the image's DDS defaults
(`etc/dds-default.json`: domain 0, no topics) and `etc/dds-types/` is its types directory: the bridge
needs a configuration, and the broker reads `$SEAMWARE_ETC_DIR/bridges.json` when `--bridgeConfig` is
not given. A `--bridgeConfig` of your own replaces it.

`coraine-build --list-features --json` prints the same list as a JSON array - `name`, `default`,
`description`, `group` (its group in `CMakeLists.txt`), `requires`: the switches it is turned off
without - each one of them - and a build setting as `NAME=value` (`HTTP_SERVER=mhd`), and `requiresAny`: the
switches it is turned off without all of - one is enough (`CHANNEL_WRITE_BACK`: `DATA_CHANNELS` or `ASSET_CHANNELS`). `--http-server builtin` builds the
broker with [corHttp](https://github.com/SEAMWARE/corHttp), the built-in HTTP server, instead of libmicrohttpd: no external HTTP library, and
the switches that need libmicrohttpd off (`POLICY_API`); not with `--pgo` or `--tune` yet. While it builds, coraine-build
prints one line per phase - `coraine-build: phase build|tools|train|install|bridges|check|done` - for a
front-end to follow.

| Option | Build |
|--------|-------|
| `--release` (default) | optimised, traces compiled out |
| `--debug` | traces compiled in (`--traceLevels`) |
| `--pgo` | profile-guided: trained on the measured request shapes, as the packages and the image ([Performance](performance.md)) |
| `--tune <workload>` | profile-guided on your workload, the knobs measured on it ([Extreme performance](extreme-performance.md)) |
| `--features NAME=ON\|OFF,...` | the `COR_FEATURE_*` switches ([Building - details](building-details.md)) |
| `--name <exe>` | the broker is `install/bin/<exe>` instead of `install/bin/coraine` - a broker built for one role, named after it: 1-64 of `A-Z a-z 0-9 . _ -`, not `coraine-import`. It reads the same `CORAINE_*` environment variables, and names itself `<exe>` in `--version` |

#### Building a plugin against it

A plugin is compiled against the headers in that tree and links none of the broker's libraries: the
broker exports its symbols and resolves the plugin's at `dlopen`
([Plugin architecture](plugin-architecture.md#how-loading-works-the-mechanism)). The headers are
included by repository, `#include "corBridge/BridgeDriver.h"`, `#include "corLog/corLog.h"`, so the
include path is the directory that holds the siblings:

```sh
mkdir ~/coraine-src
tar -xJf /usr/share/coraine/src/coraine-src.tar.xz -C ~/coraine-src --strip-components=1
cc -std=gnu11 -O2 -Wall -fPIC -I ~/coraine-src -c myBridge.c -o myBridge.o
cc -shared myBridge.o -o myBridge.so                 # + the transport's own libraries, nothing of coraine's
sudo install -m 0644 myBridge.so /opt/seamware/plugins/bridge/
coraine --bridges myBridge                            # the name of the .so, without .so
```

The MQTT and Modbus bridges in the tarball (`corMqttBridge/`, `corModbusBridge/`) are built exactly this
way - `coraine-build` runs `make -C <dir>/corMqttBridge` with `COR_LIBS ?= ..`, the parent directory as
include path - and are the templates for a bridge built outside coraine; `coraine/src/plugins/bridge/loopback`
is the reference for every entry point. A plugin built from the tarball of version X matches the
`coraine` package of version X, which was built from that same tarball. A plugin that calls Cor-Lib
functions (`COR_E`, [corJson](https://github.com/SEAMWARE/corJson), [corTree](https://github.com/SEAMWARE/corTree), ...) depends on that match; one that uses only the slots of its
contract struct depends on the contract's ABI version alone.

The packages are made by the scripts in `packaging/deb/` (`source.sh`, `build.sh`, `test-install.sh`,
`test-dev.sh`, `publish.sh`), each of which runs by hand in a container of the target distribution.

## Install with Docker

See [the docker README](https://github.com/SEAMWARE/coraine/blob/main/docker/README.md).
Build the image locally with the `Dockerfile` there; published images will land at
`quay.io/seamware/coraine:<version>-<date>-<sha>` - one immutable tag per merge, never `latest`.

## Running

The default listen port is **1026**, the default plugins are `mongoc` (current state)
and `none` (temporal), and no API plugins are loaded.

```sh
# In-memory, pretty JSON, admin API on - no external service required
coraine --database corDB --troe none --apiPlugins admin -pp 2

# MongoDB on a custom port
coraine --port 1027 --database mongoc --dbHost localhost

# Everything, including the selected plugins' own options
coraine --apiPlugins admin --database mongoc --usage
```

## Running the broker: foreground or daemon, and where the log goes

By default the broker runs in the **foreground** and writes its log to **stdout**. That is what a
container runtime (`docker logs`, `kubectl logs`), systemd (`Type=simple`, journald) and a terminal all
take the log from, and what lets them see the broker's exit: the process they started is the broker.

| Option | Environment variable | Default | Values |
|--------|----------------------|---------|--------|
| `--daemon` / `-bg` | `CORAINE_DAEMON` | off | run in the background - see below |
| `--logTo` / `-logTo` | `CORAINE_LOGTO` | `stdout` | `stdout`, `file` (`<logDir>/coraine.log`), `both` (the file and stdout) |
| `--logDir` / `-logDir` | `CORAINE_LOGDIR` | `/var/log/coraine` | the directory of the log file - read with `--logTo file` or `both` only |

A boolean environment variable is `true` or `on` (`True`, `TRUE`, `On`, `ON`), `false` or `off` (`False`, `FALSE`,
`Off`, `OFF`) - any other value, `1` included, stops the start: `CORAINE_DAEMON=true`.

### The combinations

| | `--logTo stdout` (the default) | `--logTo file` | `--logTo both` |
|---|---|---|---|
| foreground (the default) | the log on stdout | the log in `<logDir>/coraine.log`, nothing on stdout | each line in the file and on stdout |
| `--daemon` | **refused** | the log in `<logDir>/coraine.log` | **refused** |

A refused combination stops the broker before anything starts, exit code 1, with one line on stderr:

```text
coraine: --daemon with --logTo stdout: a daemon's stdout is /dev/null - use --logTo file
coraine: --daemon with --logTo both: a daemon's stdout is /dev/null - use --logTo file
coraine: --daemon and -fg contradict each other
coraine: --logTo 'syslog' is none of stdout, file, both
```

### The log file

- **Name:** `<logDir>/coraine.log` - with the defaults, `/var/log/coraine/coraine.log`.
- **The directory is not created.** It must exist and be writable by the broker's user; if the file
  cannot be opened, the broker does not start - exit code 1, and on stderr, for example:
  `Unable to open '/var/log/coraine/coraine.log' for writing: No such file or directory`.
  For a broker run as the user `coraine`: `sudo install -d -o coraine -g coraine -m 0755 /var/log/coraine`.
- **At start**, a `coraine.log` already there is renamed `coraine.log.old` (one older file is kept;
  an earlier `.old` is replaced), and the new file starts empty.
- **Permissions:** created `0664`, minus the umask - `-rw-r--r--` under the usual umask 022.
- **What is in it:** the same lines as on stdout - errors (`E:`) and warnings (`W:`) always, `-v` adds
  the informational (`I:`) and verbose (`V:`) lines, `-t` the traces of a debug build. A broker
  started with neither writes, on a quiet start, nothing at all - except, with `--daemon`, the line with
  its pid (below).
- **Rotation:** the broker does not reopen the file - it has no SIGHUP handler. Rotate it with
  logrotate's `copytruncate`: the broker writes in append mode, so after the truncation its next line
  lands at the start of the file. A `create`-style rotation (rename, then a signal) does not work: the
  broker keeps writing to the renamed file.

  ```text
  /var/log/coraine/coraine.log {
    daily
    rotate 7
    compress
    delaycompress
    missingok
    notifempty
    copytruncate
  }
  ```

  (`/etc/logrotate.d/coraine`). Lines written between logrotate's copy and its truncation are lost -
  that is copytruncate's own window.

### `--daemon`

`coraine --daemon --logTo file` detaches from the terminal - the classic daemonize:

1. `fork` - the command returns at once, **exit code 0**; the child is the broker;
2. `setsid` - a session of its own, no controlling terminal;
3. `chdir /` - the working directory is `/`;
4. stdin, stdout and stderr on `/dev/null`.

It happens after the options are checked and the log file is opened - a refused combination or a log
file that cannot be opened is still said on the terminal - and before everything else. So:

- **Exit code 0 means detached, not serving.** A start-up error after that point - a port in use, a
  store that cannot be opened - is in the log file only (`X: ... corRestStart failed on port 1026`),
  and the daemon is gone.
- **A relative path is relative to `/`** from then on: `--dbDir db` means `/db`. `--logDir` is the
  exception (the file is opened before the `chdir`). Give absolute paths.
- **There is no pid file.** The pid is in the log, on a line written with or without `-v`:

  ```text
  I: 000000.000: coraine.c[<line>]: main: --daemon: detached, pid 236587
  ```

  and `pgrep -a -x coraine` or `ss -ltnp 'sport = :1026'` find it as well.
- **Stopping it:** `kill <pid>` (SIGTERM) or SIGINT - the same ordered shutdown as in the foreground
  (requests in flight finish, the stores close; corDB on disk syncs its log and writes its snapshots).
  A signal during start-up, before the broker serves, ends it at once, and the line
  `coraine: stopped during start-up (SIGTERM / SIGINT)` goes to the log file too.
  SIGHUP is not handled: it ends the process without the ordered shutdown - do not send it.

### `-fg` / `--foreground` - deprecated

The foreground is the default, so `-fg` does nothing. It is accepted through 0.7.x and **refused from
0.8.0**; a broker started with it (or with `CORAINE_FOREGROUND=true`) writes one warning line at
start:

```text
W: 000000.000: coraine.c[<line>]: main: -fg: the broker runs in the foreground by default now - the option is refused from 0.8.0
```

(`<line>`: the source line, which changes from build to build.)

`-fg` is no longer listed by `coraine -u`. Drop it from scripts and units.

### Examples

```sh
# A terminal: the foreground, the log on the screen (Ctrl-C is the ordered shutdown)
coraine --database corDB --troe none

# A terminal, the log kept as well - in ./coraine.log
coraine --database corDB --troe none --logTo both --logDir .

# In the background, from a shell (absolute paths: the daemon's working directory is /)
mkdir -p ~/coraine-log ~/coraine-db
coraine --daemon --logTo file --logDir ~/coraine-log --database corDB --dbDir ~/coraine-db --troe none
grep -- '--daemon: detached' ~/coraine-log/coraine.log      # the pid
kill "$(sed -n 's/^.*--daemon: detached, pid //p' ~/coraine-log/coraine.log)"

# Docker: the image runs the broker in the foreground, logging to stdout - no --daemon in a container
# (the process that detaches is the container's main process: the container stops with it)
docker run --rm -p 1026:1026 coraine:local --database corDB
docker logs <container>
```

**systemd.** The package's unit ([The coraine package](#the-coraine-package)) is `Type=simple`: the
broker in the foreground, its stdout in the journal (`journalctl -u coraine`). Do not set
`CORAINE_DAEMON=true` in `/etc/default/coraine`: under `Type=simple` a broker that detaches is a main
process that exited, and systemd stops the service - and the detached broker with it.

A unit of your own that wants the log in a file uses `--daemon` with `Type=forking` - systemd then
takes the process that is left as the main one (the pid the log names):

```ini
[Service]
Type=forking
User=coraine
Group=coraine
ExecStart=/usr/bin/coraine --daemon --logTo file --logDir /var/log/coraine --database corDB --dbDir /var/lib/coraine/db
KillSignal=SIGTERM
TimeoutStopSec=120
```

A `Type=simple` unit can keep the log in a file as well, without `--daemon`: `--logTo file`
(or `both`, for the journal too).

## Configuration

Every setting is a command-line option. **`coraine --usage`** (`-u`) prints the full
list with a description of each option, and `-U` prints each option's environment variable,
type and default; the defaults of a running broker are on `GET /admin/config`. The list changes with the plugins you
select, because a plugin contributes its own options (for example `--dbHost`,
`--dbPort`, `--dbUser` come from the `mongoc` plugin).

**Every option - the broker's, each plugin's in a section of its own, coraine-import's and coraine-build's - is in
[Command-line options](options.md)**, with its variable, default, values and an example. The tables below are
the ones most deployments set.

### Core options

| Option | Default | Meaning |
|--------|---------|---------|
| `--port` / `-p` | 1026 | TCP listen port |
| `--database` / `-db` | `mongoc` | current-state plugin (short name or path) |
| `--troe` / `-troe` | `none` | temporal plugin (`none` disables history) |
| `--troeSync` / `-troeSync` | off | hand temporal writes over before the response, so a temporal read sees them at once - the response does not wait for the database |
| `--apiPlugins` / `-api` | — | comma-separated API plugins (e.g. `admin`) |
| `--pretty-print` / `-pp` | 0 | JSON indentation (0 = compact) |
| `--connectionPoolSize` / `-cps` | 0: the number of cores for a store that never waits (`corDB` or `ramDB`, `--troe` `none` or `corDB`, no `--distributed`), 32 otherwise | HTTP server thread-pool size (libmicrohttpd) |
| `--noInline` | off | hand every request to a worker thread. By default, with `corDB`, a request that waits on nothing runs on the thread that read it - about twice the throughput for retrieves and small queries. Metrics: `ngsild_requests_inline_total`, `ngsild_requests_worker_total` |
| `--memoryLimit` | 85% of the container's limit | memory budget in MiB - see [Memory budget](#memory-budget) |
| `--healthPort` | — (off) | TCP port for the health probes - see [Health port](#health-port) |
| `--policyApiPort` | — (off) | TCP port for the ODRL-PAP's `/policy` API, the policies `fdsc.so` reads - see [Authorization § 7](authorization.md#7-the-policy-api---the-broker-as-its-own-pap) |
| `--policyApiHost` | `127.0.0.1` | the address the policy API listens on - no authentication there, as on the PAP |
| `--healthStallTimeout` | 30 | seconds a request may be in flight, with no request finishing, before `GET /live` answers 503 |
| `--maxRequestSize` / `-mrs` | 2 | max request body, MiB (0 = no cap, § 6.3.2) |
| `--httpConnectionMemory` / `-hcm` | 12 | kB of memory per HTTP connection, where a request's line and headers must fit - see [Request size](#request-size) (libmicrohttpd only) |
| `--maxResponseSize` | 1/16 of the memory budget; none without one | byte budget of an entity query, MiB (0 = no budget) - see [Response size](#response-size) |
| `--entityMapMemory` | 64 | memory all EntityMaps may hold together, MiB (0 = no cap, and no automatic EntityMaps) - see [EntityMaps](#entitymaps) |
| `--distributed` / `-dist` | off | forward operations to registered Context Sources |
| `--noSplitEntities` | off | each entity lives wholly at one source |
| `--httpEndpoint` / `-he` | auto | externally reachable base URL |
| `--csourceAlias` | endpoint authority | alias used in `Via` loop detection |
| `--defaultUserContext` / `-duc` | — | default user `@context`: a URL, or a file (a path, or `file://`) - read at start and hosted as `/ngsi-ld/v1/jsonldContexts/defaultUserContext`, its URL then the default (the DDS mapping tool's `dds-context.jsonld`, as Orion-LD's `-duc` takes it). A URL that cannot be retrieved fails a request read with it - a create, a query, a retrieval, with no `@context` of its own - 504 LdContextNotAvailable, as a `Link` header's |
| `--corsOrigin` / `--corsMaxAge` | — / 86400 | CORS origin and preflight cache |
| `--distOpTimeout` / `-dtmo` | 5000 | HTTP client timeout (ms) for forwards, notifications, `@context` downloads |
| `--cooldownMillis` / `-cms` | 30000 | endpoint cooldown after a delivery failure |
| `--notifyValueChangeOnly` / `-nvco` | off | suppress value-neutral update notifications |
| `--insecureNotif` | off | accept self-signed certificates on TLS notifications |
| `--high-precision` / `-hp` | off | nanosecond timestamps instead of microsecond |
| `--asyncSnapshot` | off | run snapshot queries in the background |
| `--subStatsFlushInterval` / `-ssfi` | 60 | subscription-statistics flush interval (s) |
| `--contextSourceExtras` / `-csx` | `$SEAMWARE_ETC_DIR/contextSourceExtras.json` (`/opt/seamware/etc/...` without the variable) if it exists | JSON rendered verbatim on `/info/sourceIdentity`. A file named here and missing stops the broker; the default file missing is not an error |
| `--bridges` / `-br` | — | bridge plugins to load, comma-separated (`/opt/seamware/plugins/bridge/<name>.so`) - see [Bridges and Channels](bridge-channels.md) |
| `--auth` | — | the auth plugin (`/opt/seamware/plugins/auth/<name>.so`): `fdsc` - the Data Space Connector's access tokens and ODRL policies, with options of its own (`--fdscDiscovery`, `--fdscPap`, ...); `oidc` - an OpenID provider's access tokens and a read and a write role (`--oidcIssuer`, ...). None: every request is let in. A plugin that cannot start stops the broker - see [Authorization](authorization.md) |
| `--bridgeConfig` / `-brc` | `/opt/seamware/etc/bridges.json` if it exists | the bridge configuration file. A file named here and missing stops the broker; the default file missing means no Channels from a file. `SEAMWARE_ETC_DIR` moves the default |
| `--high-availability` / `-ha` | — | keep the caches in step with the other instances (`mongo` = change streams; needs the `mongoc` DB **and** a replica set) - see [High Availability](high-availability.md) |
| `--daemon` / `-bg` | off | run in the background - needs `--logTo file`; see [Running the broker](#running-the-broker-foreground-or-daemon-and-where-the-log-goes) |
| `--logTo` / `-logTo` | `stdout` | where the log goes: `stdout`, `file` (`<logDir>/coraine.log`) or `both` |
| `--logDir` / `-logDir` | `/var/log/coraine` | the directory of the log file (`--logTo file` or `both`) - it must exist |
| `--version` / `-V` | — | print the version and exit |
| `--traceLevels` / `-t` | — | trace levels for debugging |

### corDB on disk

`corDB` keeps the store in the broker's memory. With **`--dbDir <directory>`** it also keeps
it on disk and survives a restart: every write appends its effect to a log, synced
every 100 ms, with a snapshot as the log grows. A clean stop (SIGTERM) loses nothing, and
neither does `kill -9`, a crash or an OOM kill: the log is memory-mapped, so a record is in
the kernel's page cache before the response. A power cut loses at most the last unsynced
interval.

| Option | Default | Meaning |
|--------|---------|---------|
| `--dbDir` | none: in RAM only | the directory - one subdirectory per tenant, and the [storage format](#storage-format) |
| `--dbSync` | `interval` | `interval`: synced every `--dbSyncInterval` ms; `request`: a write answers once its record is on the disk; `none`: written, never synced |
| `--dbSyncInterval` | 100 | ms between two syncs |
| `--dbSnapshotEvery` | 64 | MiB of log after which a tenant is snapshotted (and at least as much as its last snapshot) |
| `--dbCompress` | off | the snapshots and the finished log segments compressed (zstd, loaded only then - libzstd is already in the image); the open segment never. A store written with it is read with or without it |

`--dbSync`, `--dbSyncInterval`, `--dbSnapshotEvery` and `--dbCompress` act on the disk: given without
`--dbDir` (on the command line, in the configuration file or in the environment), the broker does not
start - each one is named (`corDB: --dbSync without --dbDir - the store is in RAM only, there is no disk
for it to act on`). As `ramDB` refuses `--dbDir`, an option it does not have.

What it costs: [Performance](performance.md), "corDB on disk". How it works:
[corDB's persistence](https://github.com/SEAMWARE/corDB/blob/main/doc/persistence.md).

### ramDB - corDB in RAM only

**`--database ramDB`** is corDB built without the disk: the same store and the same API, nothing
written anywhere. It is for a deployment that wants the fastest store and can afford to lose it -
pub/sub, where a broker passes notifications on and what it holds is only ever the latest value, or
a cache in front of another system. **A restart starts empty**, by design, and nothing warns about it
at runtime: choose it knowing that.

- No disk options: `--dbDir` is an unknown option with ramDB, not one quietly ignored.
- No `--troe corDB`: it is refused (corDB keeps history inside its own store, on disk). The other
  TRoE choices work: `--troe none` (the default), `--troe timescale` for history in PostgreSQL, or
  `--troe ramDB` - a ring of the latest TRoE events in RAM, a dev/test tool today.
- Everything else - queries, subscriptions, registrations, tenants, geo-queries - is corDB's.

### TimescaleDB history (`--troe timescale`)

The temporal history in PostgreSQL + TimescaleDB, a database per tenant (`corh`, `corh_<tenant>`).

| Option | Default | Meaning |
|--------|---------|---------|
| `--troeHost` / `--troePort` | `localhost` / 5432 | the PostgreSQL server |
| `--troeUser` / `--troePwd` | `postgres` / none | its user and password (`--troePwd` is a secret: `GET /admin/config` shows `***`) |
| `--troeName` | `corh` | the database of the default tenant, and the prefix of the others |
| `--troePoolSize` | 10 | connections per tenant for the temporal reads and the Temporal API's writes |
| `--troeWriters` | 8 | writer threads per tenant, each on a connection of its own, opened the first time it writes. 0: no writers - every request writes its own history, in a transaction of its own |
| `--troeConnectTimeout` | 30 | seconds a start waits for the database, trying once a second (as `--dbTimeout` for MongoDB); 0: one try |
| `--troeDownExit` | 0 | seconds the database may be unreachable before the broker exits (code 1) - see [The database down](#the-database-down). 0: never; the broker keeps serving the current state |
| `--troeSqlIndexes` | `none` | indexes of the history beyond the basic ones, a comma list: `attrObserved`, `geo`, `time` - see [The indexes](#the-indexes-of-the-history) |
| `--troeValueIndexes` | none | the declared indexes of the history: attributes whose `q` value terms an index serves - see [Declared indexes](#timescaledb-the-history) |
| `--troeInstanceCap` / `-troeCap` | 1000000 | instances per attribute a temporal query returns without `firstN` / `lastN` |

**The writers.** A request renders the history rows of its writes and queues them for its tenant's
writers; a writer takes everything queued - up to 512 requests, 1 MiB - and writes it in one
transaction, one `INSERT` per table, each column of the rows an array (`INSERT ... SELECT ... FROM
unnest(...)`). A second writer starts on the queue when none is writing, or when 8 requests wait. Requests that arrive while it writes are the next one's. The
queue holds at most 8 MiB; past that a request waits for room.

- Without `--troeSync` (the default) a request queues its rows after its response. With
  `--troeSync` it queues them before - once its service routine is done - so a temporal read that
  follows the response sees them. No response waits for the commit.
- Measured, `--troeSync` against the default (eight cores, ABBA): writes 1-3 % fewer a second with
  50 connections, 2-6 % with one ([Performance](performance.md#--troesync-handed-over-before-the-response)).
- A temporal read, and a write of the Temporal API, of a tenant waits for every row queued before it
  to be written.
- A stop (SIGTERM, SIGINT) writes what is queued before the broker exits. `kill -9`, a crash or an
  OOM kill loses it: history rows of requests already answered, which the current state has - up to
  8 MiB, the rows of the last few milliseconds while the database keeps up.

#### The database down

The database is down when a write of the history finds no connection to it, or a server that cannot
take writes: shutting down, a standby (read-only), out of disk, out of connections (SQLSTATE classes
08, 53, 57, 58, and 25006). A write that fails for its rows - a value the database refuses - is not
that: it loses only that request's rows, and is logged.

**The current state and its subscriptions are served as before.** The history is held in memory
and retried, and recorded when the database is back - up to 8 MiB of it; past that it is lost, and
counted. Or, with `--troeDownExit`, the broker exits instead of serving on.

##### During the outage

What a client gets, measured (`ALTER DATABASE ... ALLOW_CONNECTIONS false`, its connections
terminated):

| Request | Answer |
|---|---|
| A write of the current state (POST, PATCH, PUT, DELETE, batch) | as always - 201, 204, ...; its subscriptions notify. Its history is held |
| A read of the current state | as always |
| A temporal read (`GET /temporal/entities`, `/temporal/entities/{id}`, the queries) | **500** `InternalError`, at once - `"temporal retrieve failed for entity '<id>'"`, `"temporal query failed"` |
| A Temporal API write (`POST /temporal/entities`, ...) | **500** `InternalError`, at once - `"temporal-entity create failed"` |
| An import (`coraine-import`) | its rows not written - the import reports them |
| `GET /ready` (health port) | **200** |
| `GET /live` (health port) | 200 |

**A temporal read is a 500** because the history it would read is in the database: there is no
other copy to answer from - what is held in memory is only what was written during the outage. It
does not wait for the database: a read waits for the history queued before it only while the
database is up.

**`/ready` stays 200** because the broker still serves what most requests ask for - the current state
and its subscriptions - and because every instance of a cluster writes its history to the one
PostgreSQL: a 503 for it would take every instance out of service at the same moment, and turn a
history outage into an outage of everything. `/ready` is 503 for the current-state store, not the
temporal one. The [health report](#health-port) says what is going on instead:

```json
"status": "degraded",
"troe": { "plugin": "timescale", "loaded": true, "reachable": false, ...,
          "history": { "recording": false, "notRecordingForMs": 3120, "heldBytes": 671, "lostRows": 0 } }
```

`reachable` is the once-a-second ping (an answer within the last 5 s); `history.recording` turns
false at the first write that finds the database down.

**The log** says it when it happens, as an error:

```
timescale: TRoE: the history database 'corh' cannot be reached - THE HISTORY IS NOT BEING RECORDED. Held in memory up to 8 MiB and retried; past that it is lost. The current state is served as before
```

**The history held.** The rows stay queued, in order. One writer tries again 100 ms later, then
twice as late each time - 200 ms, 400 ms, ... - up to every 5 s; the other writers wait for it. The
queue holds 8 MiB. Held for perfRun's Entity (five Attributes, ~550 bytes of JSON): 1 481 bytes for
its create, 245 for a PATCH of one of its Attributes - 8 MiB is some 5 660 creates, a quarter of a
second at the 24 025 creates/s of [the write path](performance.md#timescaledb-the-write-path), or
some 34 200 Attribute PATCHes.

**Past 8 MiB, the history is lost.** A request whose rows do not fit is answered as always, and
its rows are dropped and counted (`lostRows`). The log, at most every 10 s while rows are dropped:

```
timescale: TRoE: HISTORY LOST in db 'corh': the database cannot be reached and the history held in memory is at its cap - 12841 row(s) of history lost since the start
```

##### The database back

**The broker repairs itself; nothing is lost while the history held stayed under 8 MiB** - and,
with `--troeDownExit`, while the database came back in time. The first try that goes in ends the outage - at most 5 s after the database takes connections
again (measured: 3.2 s, after an outage of 3 s). Then:

- **Everything held is written**, in the order it was taken, in batches as always. Every row
  carries the times of its write - `modifiedAt`, `createdAt`, `observedAt` - not the time it went in:
  the history is the same as if there had been no outage.
- **Temporal reads answer again**, and see the history held: a read waits for every row queued
  before it.
- **The health report** turns back: `status` `ok`, `recording: true`, `heldBytes` 0. `lostRows` stays
  - it counts since the start.
- **The log** says how long it was gone and whether anything was lost:

```
timescale: TRoE: the history database 'corh' is back after 6 s - recording again, nothing lost
timescale: TRoE: the history database 'corh' is back after 95 s - recording again. 12841 row(s) of history were LOST meanwhile
```

**What was lost stays lost.** The current state has those writes; the history does not - an
attribute's instances skip from the last one held before the cap was reached to the first one
written after the database was back. Nothing writes them afterwards: the broker no longer has them.

A broker **stopped** during an outage (SIGTERM) tries once, and loses what it holds (logged, as
above). A broker **started** during an outage waits for the database - a try a second, for
`--troeConnectTimeout` seconds (30) - and starts once it is there; its start creates and migrates
the history's tables, so it does not serve meanwhile. Past that it does not start: exit code 1.
Only a database it cannot reach is waited for - no connection, a server still starting up, a
connection lost during the migration; one that answers and refuses (a schema version newer than
this build knows, a permission, the password of `--troeUser` / `--troePwd`) fails the start at once,
with the server's message:

```
timescale: connection to db 'postgres' refused (--troeUser, --troePwd): connection to server at "localhost" (127.0.0.1), port 5432 failed: FATAL:  password authentication failed for user "postgres"
timescale: failed to initialise the default-tenant database
```

A refused password is told from a server that is not there by the server's message, in English: a
PostgreSQL whose `lc_messages` is another language is waited for (`--troeConnectTimeout`). MongoDB
alike: a wrong `--dbUser` / `--dbPwd` fails the start at once (`mongoc: ping failed: Authentication
failed.`).

```
timescale: the history database cannot be reached - trying again once a second, for 30 s (--troeConnectTimeout)
timescale: failed to initialise the default-tenant database (30 s, --troeConnectTimeout)
```

##### `--troeDownExit`

`--troeDownExit <s>` (0, the default: never) makes the broker exit, with code 1, instead of serving
on without its history:

- **After `<s>` seconds** of the database down - counted from the first write that found it down,
  checked at each try - while there is history held. A broker with nothing to write does not exit.
- **Or at the first row that would be lost** - the 8 MiB full - whichever comes first.

It exits at once (no clean stop - no request is answered after the decision), with one of:

```
timescale: TRoE: the history database 'corh' has been unreachable for 30 s; --troeDownExit: the broker exits, 412 request(s) of history held in memory not recorded
timescale: TRoE: HISTORY LOST in db 'corh': the database cannot be reached and the history held in memory is at its cap - 3 row(s); --troeDownExit: the broker exits
```

What it costs, and what it buys:

- **The history held** - of requests already answered, up to 8 MiB - is lost at the exit: the option
  bounds the loss to the queue, it does not avoid it. The current-state stores keep what they
  acknowledged.
- **A restarted broker waits for the database** (`--troeConnectTimeout`, above), and fails again
  past it. An orchestrator that restarts it (Kubernetes, `restart: on-failure`) restarts it until
  PostgreSQL is back: with `--troeDownExit` an outage of the history is an outage of the broker. That
  is the trade - a loss bounded by the queue, and nothing served meanwhile.

##### Connections

A connection is given 5 s to connect. One whose server stops answering is given up after 30 s
without an acknowledgement (TCP keepalives after 10 s idle, 3 of them 5 s apart) - a statement that
is merely slow is not cut short. Without that, a PostgreSQL host that vanishes (a network partition,
a powered-off machine) would hold a writer for the minutes TCP takes to give up.

With `--troeWriters 0` nothing is held: a request whose history does not go in loses it (counted,
and logged), and with `--troeDownExit` the broker exits at the first one.

#### The indexes of the history

Every index costs every write: PostgreSQL updates each of them for each row. These are always there,
because the Temporal API's retrieves and queries are built on them:

| Index | Serves |
|-------|--------|
| `troe_attrs` primary key (`instance_id`, `modified_at`) | an instance by its `instanceId` (modify / delete an instance) |
| `troe_attrs` (`entity_id`, `attr_name`, `observed_at`) | an Entity's Attribute over time, `timeproperty=observedAt` (the default) |
| `troe_attrs` (`entity_id`, `attr_name`, `modified_at`) | the same with `timeproperty=modifiedAt` / `createdAt` / `deletedAt` |
| `troe_attrs` and `troe_entities` `entity_type` (GIN) | a query by `type` |
| `troe_entities` primary key (`entity_id`, `modified_at`, `op`) | an Entity's history: its existence, its types, its `createdAt` / `modifiedAt` |

The others are a choice, `--troeSqlIndexes` - for queries that go to PostgreSQL directly (a dashboard, a
report), not through the broker: the Temporal API's queries find their rows through the Entity first,
and none of them uses these.

| `--troeSqlIndexes` | The index | Serves |
|-----------------|-----------|--------|
| `attrObserved` | `troe_attrs` (`attr_name`, `observed_at`) | one Attribute of every Entity in a time window |
| `geo` | GiST on `troe_attrs.geo` (the GeoProperty values) | the rows within a distance of a point, or in an area, whatever the Entity |
| `time` | `troe_attrs` (`modified_at`) - TimescaleDB's own on a hypertable | every row in a time window |

They are applied at the start, before the broker serves: an index chosen is created, over every row
there is - on a large history that takes a while, and the writes of other brokers to that database
wait meanwhile; one not chosen is dropped. Every
broker writing to the same PostgreSQL (`--high-availability`, `coraine-import`) should run with the
same choice.

What they change, on a history of one million rows (2000 Entities, a number and a GeoProperty each,
250 instances per Attribute over ten days), best of three, ms:

| Query | the three indexes | none of them |
|-------|---:|---:|
| `GET /temporal/entities/{id}?attrs=speed&timerel=after` (a day) | 1 | 1 |
| `GET /temporal/entities?type=Vehicle&attrs=speed&timerel=after` (an hour), `limit=10` | 11 | 8 |
| `GET /temporal/entities?attrs=speed&timerel=after` (an hour), `limit=10` | 11 | 7 |
| `GET /temporal/entities?type=Vehicle&georel=near;maxDistance==5000&...&timerel=after` (an hour) | 17 | 13 |
| SQL: `attr_name = speed AND observed_at > ` (an hour) - `attrObserved` | 4 | 71 |
| SQL: `modified_at > ` (an hour) - `time` | 3 | 41 |
| SQL: `ST_DWithin(geo, <point>, 500)` - `geo` | 13 | 144 |

Without its index, a query of the last three kinds reads every row of the history - for a window of
`modified_at`, every row of the chunks (7 days each) that window falls in: it grows with the history,
the indexed one does not.
What they cost a write: [Performance](performance.md), "TimescaleDB: the write path".

### Environment variables

**Every** command-line option can also be given as an environment variable, named
`CORAINE_` + the long option in upper case, with `-` becoming `_`:

| Option | Environment variable |
|--------|----------------------|
| `--port` | `CORAINE_PORT` |
| `--database` | `CORAINE_DATABASE` |
| `--troe` | `CORAINE_TROE` |
| `--apiPlugins` | `CORAINE_APIPLUGINS` |
| `--maxRequestSize` | `CORAINE_MAXREQUESTSIZE` |
| `--maxResponseSize` | `CORAINE_MAXRESPONSESIZE` |
| `--high-precision` | `CORAINE_HIGH_PRECISION` |
| `--daemon` | `CORAINE_DAEMON` (`true` to turn it on) |
| `--logTo` | `CORAINE_LOGTO` |
| `--logDir` | `CORAINE_LOGDIR` |
| … | … |

`coraine -U` (extended usage) prints the environment-variable name of every option
alongside its type, so the list never has to be maintained by hand.
A command-line argument overrides the environment variable.

Two variables are read outside that mechanism. They move the installation's directories, and they
name no file:

| Variable | Default | Meaning |
|----------|---------|---------|
| `SEAMWARE_PLUGIN_DIR` | `/opt/seamware/plugins` | base directory plugin short names resolve against (read by the plugin loader, before the arguments are parsed) |
| `SEAMWARE_ETC_DIR` | `/opt/seamware/etc` | where the broker looks for `bridges.json` when `--bridgeConfig` is not given, and for `contextSourceExtras.json` when `--contextSourceExtras` is not given. A file missing from there is not an error |

## Storage format

Every store records the version of the format its data is written in. At start, a broker checks
it in every store it opens:

| Store | Where the version is | 0.6.0 writes |
|-------|----------------------|------------|
| `mongoc` | `<database>.metadata`, document `{ _id: "storageFormat", version: <n> }` - in the default tenant's database and in every tenant's | 2 (1 without `COR_FEATURE_Q_PREFILTER`) |
| `corDB` with `--dbDir` | `<--dbDir>/_storageFormat`: the number, as text | 1 |
| `timescale` (TRoE) | the schema version: `max(version)` of `troe_schema_version`, in every tenant's database | 3 |

- **No version recorded, no data**: a new store - this build's version is written.
- **No version recorded, data**: a store written before the version was recorded (`mongoc`:
  coraine 0.4.x or earlier). It is read as it is, and this build's version is written.
- **A version up to this build's**: the store is used. A lower version is upgraded and this build's
  is written.
- **A version above this build's**: the store was written by a newer release. **The broker does not
  start**: it exits with 1, and the error names the store, the version found and the newest this
  build knows. Run the release that wrote it, or a newer one. A tenant's `mongoc` database in a newer
  format that appears while the broker runs is not used: the request that would create the tenant is
  answered 500.

`mongoc` version 1 is the format of coraine 0.5.0: system timestamps once per entity and attribute
types once per entity. coraine 0.4.x reads that format wrong - attributes that 0.5.0 has written
come back without `type` and `createdAt` - and 0.4.x predates the check, so it cannot refuse it.
**Downgrading to 0.4.x is not supported once 0.5.0 has written to a database: do not run 0.4.x on
data 0.5.0 has written.** From 0.5.0 on, a release refuses data written by a newer one.

`mongoc` version 2 is the format of coraine 0.6.0: an attribute with an instance of a datasetId is
marked so in its document - `"@datasetIds": true`, next to its instances - for the
[q pre-filter](#q-and-the-mongoc-indexes). It is never in a response. A database in version 1 is
upgraded at start: one update of each database's `entities` collection, in the database, marks the
attributes (the log says how many entities). coraine 0.5.x refuses a version 2 database. **Every
broker on a database is stopped before the first 0.6.0 starts on it** - a rolling upgrade, with 0.5.x
and 0.6.0 serving the same database, is not supported across this version: 0.5.x writes an instance of
a datasetId without the mark, and `q` on 0.6.0 then misses it. To go back to 0.5.x, remove the marks
and record version 1, every broker stopped:

```
db.entities.find().forEach(e => {
  const u = {};
  Object.keys(e).forEach(k => { if (e[k] && (typeof e[k] === "object") && (e[k]["@datasetIds"] === true)) u[k + ".@datasetIds"] = 1; });
  if (Object.keys(u).length > 0) db.entities.updateOne({ _id: e._id }, { $unset: u });
});
db.metadata.updateOne({ _id: "storageFormat" }, { $set: { version: 1 } });
```

in the default tenant's database and in every tenant's.

The check runs in `coraine-import` too: it opens the stores the way the broker does.

### q and the mongoc indexes

A `q` value term reaches every instance of an attribute (clause 7): with `mongoc`, a `$expr` over the
attribute's instances, which no index serves. With `COR_FEATURE_Q_PREFILTER` (in the build by default)
a condition goes in front of it that an index can serve: the term on the default instance
(`<attr>.@none.value`, with its sub-attribute and value path), or the attribute marked as having an
instance of a datasetId (`<attr>.@datasetIds`). The `$expr` still decides, so the answer is the same.
With an index on each of the two, a query examines the entities that match and the ones marked, not
the collection. The operators served: `==` (a value, a range, a list), `<`, `<=`, `>`, `>=` and `~=`.
`!=` and `!~=` match an entity by a value it does not have, and read the collection as before.

### Declared indexes

An attribute whose `q` value terms an index serves is **declared** - narrowed to one entity type, to one
tenant, if so declared. A declaration is an index of the store in every tenant it covers, a tenant created
later included - `mongoc` and `corDB` (`ramDB` too) each have their own.

**What a declared index gains and costs**, per request, at full load: 100 000 entities, one attribute declared,
`--dbDir` for corDB, MongoDB 8.2 local for mongoc ([Performance](performance.md#cordbs-declared-indexes)):

| request | corDB | mongoc |
|---|---|---|
| `q` on the attribute, 10 matches | 1 111 µs -> 2.7 µs (x416) | 167 ms -> 181 µs (x919) |
| `q` on the attribute, 1 % of the entities match | 106 µs -> 6.0 µs (x17.6) | 12.3 ms -> 4.5 ms (x2.8) |
| `q` on the attribute, half the entities match (a page) | the same | the same |
| a write of the attribute (`PATCH`) | +2.8 µs (-21 %) | +2.5 µs (-5 %) |
| a create with the attribute | +3.5 µs (-48 %) | +0.7 µs (-4 %) |
| a write of another attribute | +0.13 µs (-1.3 %) | +0.3 µs (-0.6 %) |
| a create of an entity without the attribute | +0.11 µs (-3 %) | not measured |
| a replace (`PUT`) of an entity without the attribute | the same | not measured |
| a query without a `q` on it | the same | the same |

Which one weighs more is the workload's: how often the attribute is written, how often it is queried. `GET
/admin/indexes` counts both on corDB (`updates`, `accesses`), and so does `/admin/metrics`
([In /admin/metrics](#in-adminmetrics)).

Declared at start, by the store plugin's option (`mongoc`, `corDB`):

```
--dbValueIndexes https://example.org/ontology/speed,https://example.org/ontology/temperature
--dbValueIndexes '[{"attribute": "https://example.org/ontology/speed", "type": "https://example.org/ontology/Vehicle"},
                   {"attribute": "https://example.org/ontology/temperature", "tenant": "t1"}]'
```

- the attributes and entity types **expanded** - their IRIs, as the clients' @context expands them. The option has
  no @context, so a short name (no `:`) is an error: the broker does not start, and says the core @context's IRI
  of the name (`https://uri.etsi.org/ngsi-ld/default-context/speed`) - the one to use when the clients send no
  @context of their own
- IRIs, comma-separated - or a JSON array of `{ "attribute", "type"?, "tenant"? }`
- an error in it: the broker does not start
- taken out of `--dbValueIndexes`: its index is gone at the next start

Declared on a running broker, through the admin API (`--apiPlugins admin`):

| request | answer |
|---|---|
| `GET /admin/indexes` | every declaration: `id`, `attribute`, `type`, `tenant`, `declaredBy` (`dbValueIndexes` or `api`), and `tenants` - for each tenant it covers, the store's figures (below) |
| `POST /admin/indexes` `{ "attribute", "type"?, "tenant"? }` | 201, `Location: /admin/indexes/<id>`: the index built in every tenant it covers, the declaration kept in the default tenant's store. 409 if declared already, 400 for another body |
| `DELETE /admin/indexes/{id}` | 204: its index dropped in every tenant. 409 for a declaration of `--dbValueIndexes`, 404 for none |

**Declare the indexes at start** (`--dbValueIndexes`). The admin API changes them on a running broker, and
a change costs a build of the indexes; on corDB, a build of **every** declaration's, in every tenant's store,
the writes to the store waiting meanwhile. 1 000 000 entities with both attributes, one tenant, the release
build ([Performance](performance.md#building-a-declared-index)):

| | corDB | mongoc |
|---|---|---|
| the start | nothing built; the first query that can use an index builds the store's: 0.5 s | the indexes created at the tenant's setup |
| `POST` a second declaration | 0.87 s: both built again, at once; their figures from 0 | 1.1 - 2.1 s: its two MongoDB indexes; the other's untouched |
| `DELETE` it | 0.11 s: both dropped; the next query that can use the other builds it again, 0.5 s, its figures from 0 | 1 ms: its two MongoDB indexes dropped |

`<id>` is the declaration's, 8 hex digits of its attribute, type and tenant. Names in a request are expanded
with its @context (`Link` header), the core one without; a response compacts them with it. The `POST` answers
once the index is built - on a large store, that takes a while. Another broker on the same database sees a
declaration made through the API when it starts again. In the build by default;
`-DCOR_FEATURE_DB_INDEXES=OFF` leaves them out (no `--dbValueIndexes` and no `--troeValueIndexes`; `/admin/indexes` and `/admin/troeIndexes` answer 501).

#### mongoc

Each declaration is two indexes in the `entities` collection:

| index | key | partial |
|---|---|---|
| `cor_v_<id>` | `<attr>.@none.value` | on `type` when a type is declared |
| `cor_d_<id>` | `<attr>.@datasetIds` | the marked attributes only |

A query uses them through the pre-filter ([q and the mongoc indexes](#q-and-the-mongoc-indexes)): a value term
on the attribute (`==`, a range, a list, `<`, `<=`, `>`, `>=`, `~=`) and, for a declaration narrowed to a type,
`type=` that one type. The admin API's declarations are kept in the default tenant's database (`metadata`,
`{ _id: "dbIndexes" }`); a tenant another broker sets up before it starts again does not get them. An index
named `cor_v_*` or `cor_d_*` that no declaration names is dropped at a tenant's setup. `GET`'s figures per
tenant: `built`, `bytes` (both indexes) and `accesses` (the times a query used them since MongoDB started).
Without the pre-filter (`-DCOR_FEATURE_Q_PREFILTER=OFF`) mongoc has no declared indexes, and answers 501.

#### corDB

Each declaration is a value index in the tenant's store: the attribute's values - every instance's, an
array's elements - each with its entity, in order. A query whose `q` has a term on the attribute with `==`, a
range, a list, `<`, `<=`, `>` or `>=` matches only the entities with a value the term can match, in the
store's order - an AND by its term with the fewest, an OR when all its terms are served; for a declaration
narrowed to a type, the query is of that type. The answer is the one the walk of the store gives.

Built by the first query that can use it, and at once when the admin API declares one; not on disk - a
start builds it again. A change of the declarations builds every index again. The admin API's declarations
are documents of the default tenant's store (collection `dbIndexes`), on disk with `--dbDir`. `GET`'s figures
per tenant: `built`, `entries` (values), `bytes`, `accesses` (the `q` terms it served) and `updates` (the writes
that changed it: an entity with the attribute created or deleted, its values changed), since it was built.
More: corDB's [README](https://github.com/SEAMWARE/corDB#queries-by-value---declared-indexes).

#### TimescaleDB (the history)

The history has declarations of its own: `--troeValueIndexes`, and `/admin/troeIndexes`, which works the same way
as `/admin/indexes`. Each declaration is two partial indexes of `troe_attrs`, in the database of every tenant it
covers:

| index | column | partial |
|---|---|---|
| `cor_n_<id>` | `v_number` | `attr_name = '<attr>'`, and `entity_type && ARRAY['<type>']` when a type is declared |
| `cor_t_<id>` | `v_text` | the same |

```
--troeValueIndexes https://example.org/ontology/speed,https://example.org/ontology/temperature
--troeValueIndexes '[{"attribute": "https://example.org/ontology/speed", "type": "https://example.org/ontology/Vehicle"}]'
```

The option has the same format and rules as `--dbValueIndexes`. The admin API:

| request | answer |
|---|---|
| `GET /admin/troeIndexes` | every declaration (`declaredBy`: `troeValueIndexes` or `api`), then every index made in a tenant's database that serves a `q` value term (`declaredBy`: `database`, `id`: the index's name, `definition`: its `CREATE INDEX`). `tenants`: per tenant, `built`, `bytes` and `accesses` (scans since PostgreSQL's statistics were reset); a hypertable's chunk indexes are added up |
| `POST /admin/troeIndexes` `{ "attribute", "type"?, "tenant"? }` | 201, `Location: /admin/troeIndexes/<id>`: its two indexes created in every tenant whose history is open, over every row of the history. 409 if declared already, 400 for another body. An index that cannot be created: none of them kept, 500 |
| `DELETE /admin/troeIndexes/{id}` | 204: its indexes dropped in every tenant. 409 for a declaration of `--troeValueIndexes`, 404 for none |

A tenant's declared indexes are applied when its history is opened: the ones its declarations need are created,
and an index named `cor_n_*` / `cor_t_*` that no declaration names is dropped. **Declare the indexes at start**:
`POST` creates them over the whole history, in every tenant, and on a large history that takes a while. The admin
API's declarations are kept in the default tenant's database (table `troe_db_indexes`).

**Indexes made in the database** are found too. PostgreSQL keeps any index of `troe_attrs` up to date, whoever
made it, and the broker reads `pg_indexes` to know which attributes have one. These forms serve a `q` value term:

```
CREATE INDEX ... ON troe_attrs (v_number) WHERE attr_name = '<attribute IRI>'                       -- or v_text
CREATE INDEX ... ON troe_attrs (v_number) WHERE attr_name = '<IRI>' AND entity_type && ARRAY['<type IRI>']
CREATE INDEX ... ON troe_attrs (attr_name, v_number)                                               -- every attribute
```

The indexes are read when a tenant's history is opened, at every `GET /admin/troeIndexes` and `/admin/metrics`,
and by a query once the last read is 60 s old. An index made in the database is therefore used within a minute,
and an index dropped there stops being used within a minute. An index of another form is not used.

**A query** whose `q` has a term an index serves takes its entities from the index: the entities with an instance
in the time window that meets the term, each then checked by the whole `q`. The terms an index serves are a
number or string value of the attribute itself with `==`, a range, a list, `<`, `<=`, `>` or `>=`. An AND uses
the terms of it that an index serves, and an OR uses the index only when every one of its terms is served. For a
declaration narrowed to a type, the query must be of that one type. Without such a term, the query selects as
before: the latest row of every entity, each checked by `q`. That form is the faster one when no index serves
the term: the candidates form without an index reads more ([Performance](performance.md#declared-indexes-of-the-timescaledb-history)).

What it gains and costs, 100 000 entities, 2.9 million rows of history, the release build:

| | without | with the index |
|---|---|---|
| `q` on the attribute, 10 matches, a page of 20 | 10.2 requests/s, 774 ms | 638 requests/s, 12.3 ms |
| the same, `count=true` | 5.0 requests/s, 1.52 s | 605 requests/s, 13.0 ms |
| no entity matches | 10.6 requests/s, 757 ms | 11 283 requests/s, 0.7 ms |
| 1 % of the entities match, `count=true` | 9.95 requests/s, 800 ms | 161 requests/s, 49.5 ms |
| 1 % match, a page | 252 requests/s, 31 ms | 251 requests/s, 31 ms |
| half of them match, `count=true` | 12.9 requests/s, 609 ms | 13.2 requests/s, 589 ms |
| the history rows a `PATCH` of the attribute writes, per second | | -1.6 % |
| the same, a `PATCH` of another attribute | | -2.3 % |
| a batch create of 20 entities with the attribute | | -4.3 % |

The queries: temporal queries through the broker (`timerel=after`, `timeproperty=modifiedAt`), wrk with 8
connections - requests per second and the median time of one. A request also reads the history of every
entity on its page; the entity selector's SQL alone: [Performance](performance.md#declared-indexes-of-the-timescaledb-history).

#### In /admin/metrics

`GET /admin/metrics` has the figures of `GET /admin/indexes`, a series per declaration and tenant, read from the
store at every scrape:

| metric | type | mongoc | corDB |
|---|---|---|---|
| `ngsild_index_built` | gauge | 1: the indexes are there | 1: built, 0: not yet (the next query that can use it builds it) |
| `ngsild_index_entries` | gauge | - | the values in it |
| `ngsild_index_bytes` | gauge | both indexes (`$collStats`) | its size |
| `ngsild_index_accesses_total` | counter | the uses of both indexes (`$indexStats`), since MongoDB started - a query the planner answers with another index uses neither | the `q` terms it served |
| `ngsild_index_updates_total` | counter | - | the writes that changed it |

Labels: `id`, `attribute`, `type` (a declaration narrowed to one), `tenant` (`""`: the default), `snapshot` (a
Snapshot's store) - names compacted with the core @context:

```
ngsild_index_accesses_total{id="0e819997",attribute="speed",tenant=""} 1
ngsild_index_accesses_total{id="08516489",attribute="temperature",type="Room",tenant="t1"} 0
```

The history's indexes (`--troe timescale`) have the figures of `GET /admin/troeIndexes`: `ngsild_troe_index_built`,
`ngsild_troe_index_bytes` and `ngsild_troe_index_accesses_total` (scans), with the same labels. An index made in the
database has its name as `id`:

```
ngsild_troe_index_built{id="by_hand",attribute="name",tenant=""} 1
```

A declaration taken away takes its series with it. A counter starts again from 0 when its index is built
again (corDB: any change of the declarations, a start; mongoc: MongoDB's restart). A scrape asks every
tenant's store - on mongoc two aggregations per declaration and tenant.

## Migrating from another broker

Another broker's data - entities, subscriptions, registrations and temporal history, with their ids
and system timestamps - is moved into coraine's stores with `coraine-import`, before the broker is
started on them: [Migrating a database to coraine](migration.md). Orion-LD's databases are read today.

## Administration

Load the `admin` API plugin (`--apiPlugins admin`) to get:

| Endpoint | Purpose |
|----------|---------|
| `GET /admin/health` | liveness |
| `GET /admin/version` | version, git SHA, build timestamp |
| `GET /admin/log` | current log/trace levels, and `traces`: whether this build has traces compiled in (Debug and Coverage builds; a release build takes trace levels and traces nothing) — `PUT`/`POST`/`PATCH`/`DELETE` change them at runtime |
| `GET /admin/tenants` | the tenants in use |
| `GET /admin/plugins` | which plugins are loaded |
| `GET /admin/metrics` | Prometheus metrics (when compiled in) |
| `GET`/`POST /admin/indexes`, `DELETE /admin/indexes/{id}` | the declared indexes of the store ([Declared indexes](#declared-indexes)) |
| `GET /admin/config` | every option the broker runs with - its own and its plugins' - with its value, default, type, environment variable and where the value came from (`default`, `env`, `longOption`, `shortOption`, `configFile`); a plugin's under its `section`. A secret (`--dbPwd`, `--dbURI`, `--troePwd`) has `"secret": true` and its value `"***"`. What is compiled in is `GET /build`'s |
| `GET /admin/probes`, `GET /admin/probes/{name}`, `DELETE /admin/probes` | what the code decided, for the functional tests: each probe set since the start or the last `DELETE`, and its last value - `{"timescale.entitySelector": "candidates"}`; one probe that is not set: 404 ([Functional tests never read the log](testing.md#functional-tests-never-read-the-log)) |
| `GET /admin/bridges/{bridge}/inventory` | what a loaded bridge's source can carry, for a mapping tool: `?from=` where to start, in the source's terms (OPC UA: a NodeId; none: the Objects folder), `?depth=` how many levels (0-16, default 1; a bridge may cap it lower). The JSON is the bridge's own. 404: no such bridge loaded, or no such `from`; 501: the bridge has no inventory (or `COR_FEATURE_BRIDGE_INVENTORY` is off); 503: its source cannot answer (not connected, timed out) |

Log and trace levels are changeable on a running broker through `/admin/log`, which
is the intended way to debug a live instance rather than restarting it with `-t`.

## Memory budget

In a container with a memory limit (Kubernetes `resources.limits.memory`, `docker run --memory`), a
process that goes over the limit is killed by the kernel - SIGKILL, exit code 137, no warning - and
with the `corDB` store everything it held goes with it. coraine keeps a budget below the limit and
turns requests away before it gets there:

| Memory used | What happens |
|---|---|
| below 90% of the budget | everything is served |
| 90% - 100% | writes that grow memory (POST, PUT, PATCH) are refused |
| over the budget | everything is refused except what frees memory or reports on the broker: DELETE, batch delete, `/version`, `/metrics`, `/admin/*` |

A refused request gets **503**, a `Retry-After: 5` header and the error type
`https://coraine.readthedocs.io/errors/MemoryBudgetExceeded`.

The budget is `--memoryLimit` (MiB), or - without it - 85% of the smallest cgroup memory limit the
process lives under (the container's, or a Kubernetes pod's above it). Outside a container, with no
limit, there is no budget and nothing is checked.

The memory measured, every 100 ms, is the process's anonymous and shared memory (`RssAnon + RssShmem`
in `/proc/self/status`), so it includes every library the broker links; a request pays one comparison
for it. File-backed pages are not counted - the executable, the libraries and the `corDB` log, which is
memory-mapped and grows to 1 GiB a segment: they are page cache the kernel writes back and reclaims
before it kills a process, and counting them would refuse writes on a persistent `corDB` for the size
of its log.

Metrics: `ngsild_memory_budget_bytes`, `ngsild_memory_used_bytes` (what is compared with the budget),
`ngsild_memory_resident_bytes` (the whole resident set, file-backed pages included),
`ngsild_requests_refused_memory_total`.

## A store that cannot write its log

A persistent `corDB` (`--dbDir`) keeps every write in its log on the disk. When the log cannot be
written - the disk full, a file-size limit, an I/O error - the store **pauses**: from then on every
request that writes is refused, before anything is applied, so that nothing is answered 201 or 204
that a restart would lose. The broker goes on serving reads:

| Served while paused | Refused while paused |
|---|---|
| `GET`, `HEAD`, `OPTIONS` - entities, subscriptions, registrations, temporal, `/version`, `/metrics`, `/admin/*` | `POST`, `PUT`, `PATCH`, `DELETE` - entities, attributes, subscriptions, registrations, batch operations, `@context`s |
| `POST /ngsi-ld/v1/entityOperations/query`, `/temporal/entityOperations/query`, `/temporal/timepoint/entityOperations/query` | |
| `/admin/log` (the log and trace levels) | |

A refused request gets **503**, a `Retry-After: 5` header and the error type
`https://coraine.readthedocs.io/errors/PersistenceUnavailable`, with what failed in the detail:

```json
{
  "type": "https://coraine.readthedocs.io/errors/PersistenceUnavailable",
  "title": "Persistence Unavailable",
  "status": 503,
  "detail": "writes are refused until the broker can write its log again (paused for 12 s): the log of the default tenant cannot be allocated ahead"
}
```

The log is allocated 8 MiB ahead of what it holds, off the write path, so a full disk is found before
a write needs the room - unless more than 8 MiB is written between two runs of the flusher
(`--dbSyncInterval`): the store pauses with every write it answered in the log. A write
that was applied when the log failed - its record found no room, or its sync to the disk failed - is
answered 503 too; it is in memory, and the repair below keeps it.

While paused, the broker tries once a second, backing off to once every 16 s, to write a snapshot of
each tenant it could not log, from what it holds in memory; a snapshot rolls the log and drops the
files before it. Once that succeeds - the disk has room again - the pause ends by itself and writes are
served again; nothing needs a restart. A failure in the history log (`--troe corDB`) is repaired the
same way: the history of each tenant whose history log failed is written anew from memory, as new
segments that replace the old ones, and the pause ends when that is done. While it is written, temporal
reads wait for it.

`/ready` answers 503 and `status` is `paused` for as long as it lasts ([Health port](#health-port)),
`/live` stays 200: a restart loses nothing that was answered 201, but it does not make room on the
disk. Metrics: `ngsild_store_paused` (1 while paused), `ngsild_requests_refused_store_paused_total`.
Only `corDB` pauses.

## Request size

A request's **body** is limited by `--maxRequestSize` (2 MiB; 413 above it). Its **line and headers** - the
method, the URL with its query, every header - must fit in the HTTP server's memory for the connection,
**`--httpConnectionMemory <kB>`** (default 12, at least 4, at most 1024; the libmicrohttpd server only - the
built-in one ignores it):

| `--httpConnectionMemory` | headers up to | URL up to |
|---|---|---|
| 8 | 7 kB | 6 kB |
| **12** (the default) | 11 kB | 9 kB |
| 16 | 14 kB | 13 kB |
| 32 (libmicrohttpd's own default) | more than 20 kB | more than 18 kB |

A request that does not fit is refused by the HTTP server itself, before it reaches the broker: **431**
(Request Header Fields Too Large) for its headers, **414** (URI Too Long) for its URL - with the server's own
short HTML body, not an NGSI-LD error. The broker's log says it, at most once a second:

```
a request was refused with 414 by the HTTP server: its URL exceeds the connection memory (12 kB,
--httpConnectionMemory) - a long query fits in the body of POST /ngsi-ld/v1/entityOperations/query; or raise
--httpConnectionMemory
```

- **A long query** (`id=` with hundreds of ids, a long `q`): `POST /ngsi-ld/v1/entityOperations/query` takes
  the same query in its body, with no URL limit - the operation exists for gateways that take a URL of a
  kB or two.
- **Large access tokens**: a VCVerifier token is 1.5-4 kB; 12 kB leaves room for one with every other header.
- The memory is cleared at the end of every request, so it is a cost on every request: 12 kB instead of
  libmicrohttpd's 32 kB is worth 2-3 % of a core on small requests
  ([The HTTP server's connection memory](performance.md#the-http-servers-connection-memory)).

A body's **nesting** is limited too: arrays and objects at most 100 levels deep, the outermost one counted as
level 1 - an entity, its Property, and 98 levels of value. A deeper body is refused with 400
`InvalidRequest`, detail `request body nested deeper than 100 levels`, while it is parsed: the parser stops
at level 101 and goes no deeper, whatever the body's size. The limit is fixed; no option changes it.

A body's **numbers** follow JSON's grammar (RFC 8259 § 6): `+5`, `1.`, `.5`, a bare `-`, `01` and `0x1`
are refused with 400 `InvalidRequest`, detail `request body is not valid JSON`, and so is a number past the
range of a double (`1e400`). An integer that does not fit 64 bits (`12345678901234567890`) is stored as a
float; from -9223372036854775808 to 9223372036854775807 an integer stays an integer. A float is stored as
the double nearest to its text, whatever its number of decimals, so `q=a==0.3` finds the `0.3` a client
sent. A response gives a float with at most 9 decimals, or with an exponent when it is 1e21 or more, or too
small for 9 decimals to show.

## Response size

**`--maxResponseSize <MiB>`** (0 = no budget, at most 4096) is the byte budget of an entity query: `GET /ngsi-ld/v1/entities` and `POST /ngsi-ld/v1/entityOperations/query`, on the live
tenant and on a Snapshot - and of the [temporal queries](#temporal-queries) and the
[forwarded part](#distributed-queries) of a distributed query. The store counts each entity as it fetches it and stops before the one that
would take the sum past the budget, so the memory a query takes is bounded while it is fetched, not
after it has been rendered. What is counted is the stored entity: with `mongoc` the length of its BSON
document, with `corDB` the size of its JSON rendering. `pick`, `omit`, `attrs` and the output format
do not change it.

Without the option, the budget is **1/16 of the [memory budget](#memory-budget)** - so it follows a
container's memory limit (or `--memoryLimit`): 27 MiB in a 512 MiB pod, 108 MiB in a 2 GiB one - and
a broker with no memory budget (no limit, no `--memoryLimit`) has none. A response holds more than its
bytes (the entities fetched, the rendered body, its send buffer) and several run at once; a sixteenth
leaves the memory budget room for them. `-v` at startup says which budget is in force and why.

`limit` keeps its meaning. The two are independent bounds, and whichever binds first ends the page:

| Case | Answer |
|---|---|
| the budget is reached before `limit` | 200 with a page shorter than `limit`. `Link` `rel="next"` points at `offset` + the entities returned, with the same `limit`; `NGSILD-Results-Count` (`count=true`) is the size of the whole result set |
| the first entity of the page alone is larger than the budget | 403, error type `https://uri.etsi.org/ngsi-ld/errors/TooManyResults` |
| the query needs every match at once - `orderBy` (the matches are ordered before they are paginated) - and they are larger than the budget | 403 `TooManyResults` |
| a page served from an [EntityMap](#entitymaps) | the page ends before the entity that would pass the budget, `rel="next"` points at the map position after the last one on it; 403 only when the first entity alone is larger |

The end of a result set is the page without a `rel="next"` link, not a page shorter than `limit`.

### Temporal queries

The same budget bounds `GET /ngsi-ld/v1/temporal/entities`, `POST /ngsi-ld/v1/temporal/entityOperations/query`
and `GET /ngsi-ld/v1/temporal/entities/{id}`. The store measures an entity's instances before it fetches
them: with `timescale` the text of each instance's values plus 128 bytes for its type and timestamps,
with `corDB` the stored record of each instance. The instances are taken by rank - the k-th instance
of every attribute (and datasetId) of the page, in the page's order - so a cut leaves every attribute
with the same number of instances, as a smaller `firstN` / `lastN` would.

| Case | Answer |
|---|---|
| a query: the budget is reached before `limit` | 200 with a page shorter than `limit`, each entity on it with every instance its temporal page gives it. `Link` `rel="next"` points at `offset` + the entities returned |
| one entity whose instances are larger than the budget - a retrieve, or the first entity of a query's page | 200 with the K instances per attribute that fit (K smaller than `firstN` / `lastN` / `--troeInstanceCap`). The temporal pagination `Link` (`rel="intervalafter"`, `rel="intervalbefore"` with `lastN`) points at `offsetN` + K, as for the instance cap (TS 104-176 § 6.4.7.3). On a query, that entity ends the page: `rel="next"` points at `offset` + 1 |
| not even one instance of every attribute of the entity fits | 403 `TooManyResults` |
| `format=aggregatedValues` computed by the store | each entity whole or not at all (a bucket is not an instance that can be left for the next page): a shorter page, or 403 when the first entity is larger than the budget |

`--troe ramDB` answers no temporal query, so it has nothing to bound.

### Distributed queries

What the Context Sources answer to a forwarded query counts against the same budget, in
`GET /ngsi-ld/v1/entities`, `POST /ngsi-ld/v1/entityOperations/query` and the temporal queries above.

Each source's answer is read up to the budget and no further: an answer whose body is larger is
dropped as it arrives, its connection closed, and the query is refused with 403 `TooManyResults`
naming the registration - nothing of it can be placed on a page. Lowering `limit` makes the
source's answer smaller. Over HTTP the read stops at the budget; an answer from a source this broker
serves in process, or over `cor://`, is already in memory when it is measured, and is dropped the same.

The answers that were read are then counted entity by entity, as rendered:

| Case | Answer |
|---|---|
| a paged query (`splitEntities=false` or `--noSplitEntities`: every entity wholly at one source) | the local page and each source's page are cut at the same depth - the most entities of each that fit in the budget together, and in `limit`. 200, `Link` `rel="next"` at `offset` + that depth: the sources are asked for the same `offset` and `limit` as the local store, so the next page continues every one of them where this one stopped |
| the depth is 0: the first entity of the local page and of each source's page together are larger than the budget | 403 `TooManyResults` |
| a query that needs every match at once - `orderBy`, and split entities (the default: an entity is assembled from the parts the sources hold before the page is cut) without an [EntityMap](#entitymaps) - and the local and forwarded matches together are larger than the budget | 403 `TooManyResults`. With automatic EntityMaps (the default) a split-entity query over the budget is paged through a map instead |
| one entity's history, `GET /temporal/entities/{id}`, with Context Sources | the instance arrays of every attribute - local and each source's - cut at the same depth (the K instances of each that fit together); `rel="intervalafter"` at `offsetN` + K |

In the build by default; `-DCOR_FEATURE_RESPONSE_BUDGET=OFF` leaves it out (no option, no budget).

## Pagination

A query whose answer is more than `limit` entities is paged; its `Link` response header points at the
next page (`rel="next"`) and, from the second page on, the previous one (`rel="prev"`), each a complete
request: every parameter of the query, `limit`, and where the page starts (TS 104-175 § 7.4.2.2,
TS 104-176 § 6.4.7.2).

**A local query** (no registration matches it, or `local=true`) in the default order (no `orderBy`) is
paged by **position**: `next` names the last entity of the page, `prev` the first -

```
Link: </ngsi-ld/v1/entities?type=T&q=speed%3E0&limit=20&pageAfter=1791549752292631675,urn:E20>;rel="next";type="application/json"
```

| parameter | the page |
|---|---|
| `pageAfter=<createdAt>,<id>` | the first `limit` matches after that entity, in the default order (creation time, then id) |
| `pageBefore=<createdAt>,<id>` | the last `limit` matches before it |

The value is the entity's creation time as the store holds it (an integer, nanoseconds) and its id - a
position given by the broker's own links, not to be built by a client (it may change between
versions). The store reads from the position on: MongoDB an index range on `{type, createdAt, _id}`
(or `{createdAt, _id}`) - no skip; corDB the entity's place in its list, found by its id (one hash
lookup), and walking back one lookup per entity for `pageBefore`. A page deep in the result costs what
the first page costs, where `offset` reads (MongoDB) or walks (corDB) every match before it.

What a page by position does that `offset` does not: an entity deleted, or one that no longer matches,
on an earlier page moves nothing - the next page starts right after the previous page's last entity,
with no entity skipped and none repeated (with `offset`, one entity fewer before the page shifts every
later page by one, and one entity is never seen). The position's own entity deleted: the page starts
where it was (its creation time; corDB walks its list to it then, as `offset` does). It is a live
result, not a frozen set: an entity that starts to match before the position is not on the pages after
it, and the pages after it show the entities as they are now. A frozen set is an EntityMap
(`entityMap=true`, below).

- A client's own `offset` pages as before, and the links of its pages carry `offset`.
- `orderBy`, `georel=near`, a query that Context Sources answer too (distributed), an EntityMap, a
  Snapshot (`NGSILD-Snapshot`): paged by `offset` (or the map's offset), as before.
- `pageAfter` / `pageBefore` with `offset`, with `orderBy`, with `georel=near`, with an EntityMap or a
  Snapshot, both together, or a value that is not a position: 400 `BadRequestData`. A position on a
  query that has become distributed since its first page (a registration matches it now): 400 - its
  pages start over.
- `NGSILD-Results-Count` (`count=true`) is the count of the whole query, on every page.
- The links are `next` and `prev` only - no `first` / `last` (the first page is the query without a
  position).

The names are coraine's: TS 104-175 § 7.4.2.3 leaves the links opaque ("only requires pointers to the
following and previous pages, which can be implemented in a completely opaque way"); `limit` and
`offset` are the "transparent" option, and a client using them gets them.

### An offset past the end

An `offset` at or past the end of the result set gives its **last** entity, not an empty page
(TS 104-176 § 6.4.7.2; NGSI-LD 1.9.1 Table 6.3.10-2: "only the last element of the result set is to be
returned"), and the answer says so:

```
NGSILD-Warning: 199 coraine "offset 50 is past the result set (5 entities) - the last entity is returned"
Link: </ngsi-ld/v1/entities?type=T&limit=10&offset=0>;rel="prev";type="application/json"
```

- 199 is Miscellaneous Warning: the spec has no code of its own for this (its warning codes,
  NGSI-LD 1.9.1 § 6.3.17, are the distributed operations'); `coraine` is the warn-agent.
- `prev` is the page before the last entity (its offset minus `limit`, 0 at the least), not the page
  before the offset asked for, which is past the end too.
- The same for `orderBy`, `POST /entityOperations/query`, a distributed query with split entities (the
  offset is applied to the entities assembled here) and an EntityMap (past the end of its frozen set).
- An empty result set: `[]`, no warning. A distributed query with `splitEntities=false`: the local
  store and each source apply the offset to their own matches, and this broker adds no warning - a
  source that is a coraine answers its own last entity, and its warning is not passed on.

### A distributed query: count, offset, limit=0

A query that Context Sources answer too (registrations match it, `--distributed`):

| | split entities (the default) | `splitEntities=false` / `--noSplitEntities` |
|---|---|---|
| what the sources are asked | `type`, `id`, `idPattern` - every candidate; the filters are applied to the entities assembled here | the query as it is, with `limit`, `offset` and `count` (in the URL of `POST /entityOperations/query` too) |
| `NGSILD-Results-Count` (`count=true`) | the number of entities assembled and filtered here | the local count plus each source's `NGSILD-Results-Count` (a source that sends none: the entities it answered), an entity the answers show in more than one place counted once |
| `offset` | applied to the assembled entities | applied by the local store and by each source to its own matches |
| `limit=0` | count-only: the count, and `[]` | the same - and the sources are asked `limit=0&count=true` |

TS 104-176 § 6.4.6 asks for "the total number of matching results"; with split entities every match is
assembled here, so the count is exact as far as the sources' answers go - a source answers a query
without `limit` with its default page. Without split entities the sources count their own matches;
an entity held in more than one place is counted once only when it is on the pages answered, so the
count can be higher than the number of distinct entities when the result is more than one page. A
query whose first page has more after it gets an [automatic EntityMap](#entitymaps), and its count is
the size of the map: every match, each counted once.

## EntityMaps

An EntityMap (TS 104-175 § 9.6) freezes the **set** of entities a query matches - their ids, in the
query's order, each with the sources that hold it - and a paginated query is served from it: a page
is a slice of the frozen ids, each entity fetched from where the map says it lives. It freezes the
set, not the values. Every page applies the query's filters (`type`, `q`, `scopeQ`, the GeoQuery)
again, and an entity that no longer matches is left out: `limit=20` with 3 that no longer match is a
page of 17, and the next page still starts 20 further on (see below). An entity created after the map
is not in it.

**Automatic EntityMaps.** A `GET /ngsi-ld/v1/entities` whose first page (`offset` 0) has more after it
gets a map without asking for one, as `--autoEntityMaps` says:

| `--autoEntityMaps` | |
|---|---|
| `distributed` (the default) | a distributed query - the sources are not asked for `offset` / `limit`, so without a map there is no correct second page |
| `all` | a local query too, when the store holds more than `limit` matches |
| `none` | none - a map only when a client asks for one |

A local query pages in the store ([Pagination](#pagination)), and an automatic map makes its first page
cost more: a second query for the ids of every match, and the memory of the map
([performance.md](performance.md#what-automatic-entitymaps-cost) has the numbers - a first page of
20 of a 1 % type in 300 000 entities: from 11 725 to 574 requests/s on MongoDB, from 16 372 to 66 on
corDB). What it buys is pages that are slices of the set as it was at the first page; a local query's
pages by position already skip and repeat nothing. A client that wants the frozen set for a local
query asks for it: `?entityMap=true`.

- a local query (`all`), when the store holds more than `limit` matches;
- a distributed query (`distributed`, `all`) (registrations matched, `--distributed`), when its answer is more than a page -
  more than `limit` local matches, a source that answered a whole page, or more than `limit` together.
  The broker then asks each source for its own EntityMap (`GET /ngsi-ld/v1/entityMaps` on the source)
  and records it (`linkedMaps`); the pages fetch each entity from its sources, asking a source that
  answered with a map for its entity from that map. With split entities (the default) the sources are
  given no filters (a part of an entity cannot be checked), so what they answer are candidates: when the
  map is created, the broker assembles them and keeps the ones that match `q`, `scopeQ` and the GeoQuery
  (TS 104-175 § 7.4.2.4) - a hundred at a time, the cost of the query's first page with a filter. The map
  holds the matches, and `NGSILD-Results-Count` is their number; each page applies the filters again.

Not for: a query of one page (nothing to paginate - no map, and the sources are asked exactly what a
query without maps asks them), a query that starts at another `offset`, `limit=0`, `POST
/entityOperations/query`, and `orderBy`: ordering by attribute values needs the values frozen, which
is a Snapshot (`NGSILD-Snapshot`), not an EntityMap.

| | Status | `NGSILD-EntityMap` response header | `Link` |
|---|---|---|---|
| a query that gets an automatic map | 200 - the answer is the result of the query; the map is a by-product of paginating it | the map's URI | `next` / `last` name the map |
| a page of an expired or unknown map, with the query's parameters | 200 - a new map is made from them | the new map's URI | name the new map |
| `?entityMap=true`, or `GET`/`POST /ngsi-ld/v1/entityMaps` | 201 Created (TS 104-176 clause 7: "in case an EntityMap has been (re)created") | the map's URI | the first page's `next` / `last` name the new map |
| a page of an existing map: `?entityMap=<id>`, or the request header `NGSILD-EntityMap: <URI>` | 200 | the map's URI | `first` / `prev` / `next` / `last` name the map |

Every link names the map - `entityMap=<id>` with `limit` and `offset` - and repeats the whole query
that created it: the selecting parameters (`type`, `q`, `scopeQ`, the GeoQuery, `attrs`, `id`, `csf`,
`local`, ...) and the ones that shape the answer (`options`, `format`, `pick`, `lang`, `count`, ...).
TS 104-175 § 9.6: "Subsequent requests referencing an Entity Map shall use the same parameters as in
the original request that created the Entity Map, except for the specification of Entity identifiers
or parameters related to pagination". So a followed link is a complete query by itself. A page that
sends a selecting parameter with a different value than the map's query, or one that query did not
have, is 400 `BadRequestData` naming the parameter; one that leaves a selecting parameter out is served
by the map's query. The request header `NGSILD-EntityMap` and `?entityMap=<id>` naming different maps
is 400. `NGSILD-Results-Count` (`count=true`) on a page of a map is the size of the frozen set.

An entity that no longer matches is left out of its page, and stays in the map. § 9.6 says such
entities "shall be removed from the Entity Map"; the pages are the same either way, except that a
removal would shift every later position, and the next page, at `offset` + `limit`, would skip as many
entities as were removed. So the positions of the frozen set never move, a page can be shorter than
`limit`, and `NGSILD-Results-Count` stays the size of the frozen set.

A map lives 5 minutes. An automatic map's lifetime slides: each page served from it gives it another
5 minutes. A page of a map that has expired, or is unknown: "a new one shall be created" (§ 9.6) - from
the request's own parameters (a link carries the whole query), and the requested page is served from
the new map, named in `NGSILD-EntityMap` (200). A request with no selector at all - nothing to make a
map from - gets 404.

**`--entityMapMemory <MiB>`** (default 64) is the memory all EntityMaps may hold together: an entity
id and its sources, about 70 bytes plus the id for an entity of this broker. When a new map does not
fit, the expired maps go, then the automatic maps used longest ago; a map a client asked for is never
evicted. An automatic map that still does not fit is not made - the query is paginated as it would be
without one (`offset` / `limit`, local and forwarded); a map the client asked for is refused with 403
`TooManyResults`, as is a source's EntityMap larger than the whole cap. `0` removes the cap and turns
automatic maps off. `ngsild_entity_map_bytes` and `ngsild_entity_map_store_size` (`/admin/metrics`)
show what the maps hold.

A page of a map fetches the page's entities of this broker in one call to the store (MongoDB: one
query, `{_id: {$in: [...]}}`; corDB: the id index), and the ones of each Context Source from it.

In the build by default; `-DCOR_FEATURE_AUTO_ENTITY_MAP=OFF` leaves the automatic maps and
`--autoEntityMaps` out (a map is then only made when a client asks for one).

## Health port

With **`--healthPort <port>`** the broker answers health probes on a port of its own, served by a
thread of its own - not by the HTTP server, so a probe never waits in its queue behind the requests.
Without the option nothing listens. It opens before the store is loaded.

| Request | 200 | 503 |
|---|---|---|
| `GET /live` | the broker makes progress | a request has been in flight for longer than `--healthStallTimeout` seconds (default 30) and no request has finished in that time. Never for memory or a database |
| `GET /ready` | the broker serves | the store is still loading; the current-state store does not answer its ping; over the [memory budget](#memory-budget); the store [paused](#a-store-that-cannot-write-its-log); stuck (as `/live`); stopping (from SIGTERM on) |
| `GET <anything else>` | always | — |

A connection that sends nothing (a TCP probe) is closed unanswered. Every answer has the same body,
the report:

```json
{
  "status": "ok",
  "uptime": 3605,
  "store": { "plugin": "mongoc", "loaded": true, "reachable": true, "lastPingAgoMs": 412, "pingLatencyUs": 310 },
  "troe": { "plugin": "none", "loaded": true, "reachable": true },
  "memory": { "budget": 912680550, "used": 61435904, "resident": 75988992, "level": "ok", "refused": 0 },
  "requests": { "inFlight": 2, "total": 1288211, "lastFinishedAgoMs": 0 }
}
```

| Member | |
|---|---|
| `status` | `starting` (the store loading), `ok`, `degraded` (the temporal store unreachable or not recording, or over 90% of the memory budget), `down` (the store unreachable, or stuck), `paused` (the store [paused](#a-store-that-cannot-write-its-log)), `stopping` |
| `uptime` | seconds since the health port opened |
| `store`, `troe` | the current-state and the temporal plugin. `mongoc` and `timescale` are pinged once a second, on a connection of their own, with a 1 s timeout: `reachable` is an answer within the last 5 s, `lastPingAgoMs` and `pingLatencyUs` are the last ping's. `corDB` and `none` are in the broker's process: reachable once loaded |
| `troe.history` | `--troe timescale` only: `recording` (false: the database is down - [The database down](#the-database-down)), `notRecordingForMs`, `heldBytes` (history taken and not written yet) and `lostRows` (since the start) |
| `memory` | the [memory budget](#memory-budget) in bytes (0: none), what counts against it, the whole resident set, `level` (`ok`, `soft` - writes refused, `hard` - everything but deletes and monitoring refused) and the requests refused for it |
| `paused` | only while the store is [paused](#a-store-that-cannot-write-its-log): `forS` (seconds since the pause began), `reason` and `refused` (the requests refused for pauses since the start) |
| `requests` | the requests in flight, how many have finished, and how long ago the last one did (`null`: none yet) |

Nothing is measured when a probe arrives: the figures are kept current in the background, and an
answer formats them. In the build by default; `-DCOR_FEATURE_HEALTH=OFF` leaves it out.
How the probes are set up on Kubernetes: [Kubernetes](kubernetes.md#probes).

## Sanity check procedures

The steps below verify that a new installation is complete and working. They assume
the defaults - port **1026**, the `mongoc` plugin on a local MongoDB - and the
`admin` API plugin loaded (`--apiPlugins admin`). Adjust host and port to match.

### End-to-end test

Create an Entity, read it back and delete it. Each step names the answer that means
"working".

```sh
# 1. The broker answers, and says which build it is  -> 200, "coraine version" and each library's version
curl -s localhost:1026/admin/version

# 2. Create an Entity  -> 201 Created, with a Location header
curl -si localhost:1026/ngsi-ld/v1/entities -H 'Content-Type: application/json' \
     -d '{"id": "urn:ngsi-ld:SanityCheck:1", "type": "SanityCheck",
          "temperature": {"type": "Property", "value": 21.5}}'

# 3. Read it back  -> 200, the same Entity, "temperature" with value 21.5
curl -s localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:SanityCheck:1

# 4. Delete it  -> 204 No Content; a second GET then answers 404
curl -si -X DELETE localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:SanityCheck:1
```

With `--database corDB` the same steps apply; the Entity lives in memory instead.

### List of running processes

One process, `coraine`:

```sh
pgrep -a -x coraine              # a native installation
docker ps --filter name=coraine  # the Docker image
```

It runs in the foreground by default; `--daemon` detaches it - see
[Running the broker](#running-the-broker-foreground-or-daemon-and-where-the-log-goes).

### Network interfaces up and open

- **TCP 1026** (or the `--port` given), for NGSI-LD and the admin API:
  `ss -ltnp | grep 1026`
- **TCP 27017** to MongoDB, outgoing, when the `mongoc` plugin is used.

### Databases

With the `mongoc` plugin, the default tenant is the database named by `--dbName`
(default `cor`); every other tenant has a database of its own. After step 2 of the
end-to-end test the database exists:

```sh
mongosh --quiet --eval 'db.getMongo().getDBNames()'
```

`corDB` keeps its state in the broker process and needs no database - with
`--dbDir`, in that directory (`snap-N.cor`, `log-N.cor`, per tenant).

### Diagnosis

- **The log:** stdout by default - the terminal, `docker logs`, `journalctl -u coraine` for the
  package's service; with `--logTo file` or `both`, `<logDir>/coraine.log` (default
  `/var/log/coraine/coraine.log`). Errors (`E:`) and warnings (`W:`) are always written; `-v` adds
  the rest.
- **Liveness:** `GET /admin/health`. **Loaded plugins:** `GET /admin/plugins`.
- **More detail from a running broker:** raise the trace levels through
  `/admin/log` (see [Administration](#administration)) instead of restarting it with `-t`.
- **The broker does not start:** run it in the foreground (no `--daemon`) with the default
  `--logTo stdout`, which prints the reason on the terminal. A MongoDB that cannot be reached is the
  most common cause.

## Multi-tenancy

Tenants are selected per request with the `NGSILD-Tenant` header. With the `mongoc`
plugin each tenant is a separate database; with `corDB` each tenant is a separate
store (and, with `--dbDir`, a directory of its own). No configuration is needed to create one: a request
that creates something on an unknown tenant creates the tenant too - Create Entity, Batch Create, Batch
Upsert, Create Temporal Evolution, Create Subscription, the context source registrations and
subscriptions, and the creations of Snapshots, Service Registrations, Combined Service Templates, Service
Executions and EntityMaps. Any other request on a tenant that does not exist - a query, an update, a
PATCH, a DELETE - answers **404 NonexistentTenant** and creates nothing.
