# coraine — Plugin architecture

> **This is the heart of coraine.** The broker binary contains the NGSI-LD
> protocol logic, the REST layer, the JSON-LD engine and the subscription
> matcher. It contains **no storage code and no temporal code**. Those — plus any
> non-NGSI-LD admin/ops endpoints — are dynamically loaded shared objects. You
> choose them at startup; you can write your own without touching the core.

How it came about: [history](history/plugin-architecture.md).

## The six plugin categories

There are **six** kinds of plugin:

- **Current-state DB** — where entities, subscriptions and registrations live.
  Loaded via `--database` / `-db`; resolves to `<base>/db/currentState/<name>.so`;
  register symbol `dbRegister`; fills the `DbDriver` struct (`db`). **One active at
  a time.** Bundled: `mongoc` (default), `corDB`.

- **History DB** — the temporal evolution of entities (TRoE — Temporal
  Representation of Entities). Loaded via `--troe` / `-troe`; resolves to
  `<base>/troe/temporal/<name>.so`; register symbol `troeRegister`; fills the
  `TroeDriver` struct (`troe`). **One active at a time** (`none` disables history).
  Bundled: `none` (default), `timescale`, `ramDB`, and `corDB`.

  `--troe corDB` has no `.so` under `troe/temporal/`: corDB keeps the history inside
  its own store - the same lock, the same log - and the loader takes the TRoE driver
  from the current-state `corDB.so`'s own `troeRegister`. It therefore needs
  `--database corDB`; `--database mongoc --troe corDB` (or `ramDB`) is refused at
  start.

- **API services** — extra HTTP endpoints beyond the NGSI-LD core (ops, admin,
  health, …). Loaded via `--apiPlugins` / `-api`; resolves to `<base>/api/<name>.so`;
  register symbol `apiRegister`; fills an `ApiPlugin` entry. **Any number** active
  (comma-separated, up to `API_PLUGINS_MAX = 16`). Bundled: `admin`.

- **Communication protocols** — the transport over which the broker speaks to
  clients and to other brokers. **REST/HTTP** and **`cor://`**, the binary protocol
  (`--corPort`; [The cor format and cor://](cor-protocol.md)), are built into [corRest](https://github.com/SEAMWARE/corRest).
  **Transport plugins** carry the API over another protocol: loaded via `--transports`;
  resolves to `<base>/transport/<name>.so`; register symbol `transportRegister`; fills a
  `TransportDriver` (`src/lib/plugin/TransportDriver.h`). Up to `TRANSPORTS_MAX = 4`.
  Bundled: `ws` - WebSocket, upgraded from `GET /ngsi-ld/v1/ws` on the HTTP port
  ([WebSocket](websocket.md)). cor:// is to become one.

- **Bridges** — the transports over which the broker speaks to something that is
  **not** an NGSI-LD client: a DDS topic, an MQTT broker, an OPC-UA server.
  Loaded via `--bridges` / `-br`; resolves to `<base>/bridge/<name>.so`;
  register symbol `bridgeRegister`; fills a `BridgeDriver`. **Any number** active
  (comma-separated, up to `BRIDGES_MAX = 8`). Bundled: `loopback`. External, each
  in its own repo and built by the [corLibs](https://github.com/SEAMWARE/corLibs) umbrella:
  [`corDdsBridge`](https://github.com/SEAMWARE/corDdsBridge),
  [`corModbusBridge`](https://github.com/SEAMWARE/corModbusBridge) and
  [`corMqttBridge`](https://github.com/SEAMWARE/corMqttBridge). Beside them, built only when
  `coraine-build --bridges` names them:
  [`corCkanBridge`](https://github.com/SEAMWARE/corCkanBridge) and
  [`corOpcuaBridge`](https://github.com/SEAMWARE/corOpcuaBridge) (open62541).

  ⭐ This is a *different axis* from the communication protocols above, and the
  two are easy to confuse. A communication protocol is how a **client reaches
  the broker**. A bridge is how the **broker reaches a wire**. Both will exist,
  and neither replaces the other.

  The contract lives in its own library,
  [`corBridge`](https://github.com/SEAMWARE/corBridge), rather than here —
  `corPlugin` is the precedent: one is the *mechanism* for loading a `.so`, the
  other the *contract* one kind of `.so` must satisfy.

- **Auth** — who sent a request and whether it may do what it asks. Loaded via
  `--auth`; resolves to `<base>/auth/<name>.so`; register symbol `authRegister`;
  fills an `AuthDriver` (`src/lib/plugin/AuthDriver.h`, plain data, `AUTH_ABI_VERSION`).
  **One at most**; none = every request let in. A plugin that cannot load or init
  stops the broker. Bundled: `fdsc` - the FIWARE Data Space Connector's tokens and
  policies - and `oidc` - an OpenID provider's access tokens and their roles
  ([Authorization](authorization.md)).

All six are live.

## What crosses a bridge, and what does not

Two properties, and everything else about bridges follows from them.

**A bridge knows nothing about entities.** It carries bytes to and from an
*endpoint*. Which entity attribute an endpoint corresponds to is a **Channel**,
and Channels live in the broker. So the seam speaks `(endpoint, json, time)` in
both directions and nothing else: inbound through `BridgeBroker::sampleIn`,
outbound through `BridgeDriver::publish`.

**Everything crossing it is plain data** — `const char*`, `int64_t`. No
`KjNode`, no `CorAlloc`, no NGSI-LD type. That is not a style preference:

- a bridge plugin need not be a C program. The DDS one *cannot* be: the DDS
  Enabler's API takes `std::string` and `std::shared_ptr`, which no amount of
  declaring mangled symbols reaches from C. It is C++, compiled separately, and
  the broker — still `PROJECT(coraine C)` — `dlopen`s it and never sees a C++
  token.
- calls arrive on **threads the broker did not create**. Handing a [corAlloc](https://github.com/SEAMWARE/corAlloc) buffer
  across that line would be a bug the day it was written.

⭐ There is also no loop, and not because of a guard: an arriving sample is
stored by a path that writes through the DB driver and never enters a service
routine, while the outbound hook is *in* the service routines. A value that came
in from a transport structurally cannot reach the code that sends values out.

## Where plugins are loaded from

The base directory defaults to **`/opt/seamware/plugins`** and is overridable by
the **`SEAMWARE_PLUGIN_DIR`** environment variable
(`corPluginSetBaseDir("/opt/seamware/plugins", "SEAMWARE_PLUGIN_DIR")` in
`coraine.c`). `make install` copies the bundled plugins into this tree:

```
/opt/seamware/plugins/
├── db/currentState/
│   ├── mongoc.so          # MongoDB-backed store
│   ├── corDB.so         # in-process store, in RAM or on disk (--dbDir)
│   └── ramDB.so         # the same, in RAM only
├── troe/temporal/
│   ├── none.so            # no-op (temporal disabled)
│   ├── ramDB.so           # a ring of the latest TRoE events in RAM (dev/test)
│   └── timescale.so       # TimescaleDB/Postgres history
├── api/
│   └── admin.so           # health/version/log/tenants/plugins
├── auth/
│   ├── fdsc.so            # the Data Space Connector's tokens and policies (--auth fdsc)
│   └── oidc.so            # an OpenID provider's access tokens and roles (--auth oidc)
├── transport/
│   └── ws.so              # WebSocket (--transports ws)
└── bridge/
    ├── loopback.so        # a transport that goes nowhere - test instrument
    ├── dds.so             # DDS, from corDdsBridge (not built by default)
    ├── modbus.so          # Modbus TCP, from corModbusBridge
    ├── mqtt.so            # MQTT, from corMqttBridge
    ├── ckan.so            # CKAN, from corCkanBridge (coraine-build --bridges ckan)
    └── opcua.so           # OPC UA, from corOpcuaBridge (coraine-build --bridges opcua)
```

A plugin can also be given as a **full path** (any argument containing a `/`),
which bypasses base-dir resolution — handy for pointing at a freshly-built `.so`
in a build tree without installing:

```sh
coraine --database ../corDB/obj/debug/corDB.so
```

## How loading works (the mechanism)

`src/lib/plugin/pluginLoader.c` does, per plugin:

1. `corPluginResolve(base, category, subcategory, name, path, …)` → builds the `.so`
   path (skipped when `name` already looks like a path).
2. `corPluginOpen(path, "<symbol>", …)` → `dlopen` + `dlsym` for the register symbol
   (`dbRegister` / `troeRegister` / `apiRegister` / `bridgeRegister` / `transportRegister` /
   `authRegister`). Handles are
   tracked for
   `corPluginCloseAll()` at shutdown.
   A plugin built against another `CorNode` layout than the broker is refused here, whatever
   its kind ([The CorNode layout](#the-cornode-layout)).
3. For a DB or TRoE plugin, its interface stamp is checked ([The DB plugin interface
   stamp](#the-db-plugin-interface-stamp)) - before the register function is called.
4. The register function is called with a zeroed driver struct, which it fills with
   its function pointers.

Plugins do **not** statically link the NGSI-LD/Cor-Lib symbols — the broker is linked
`rdynamic` (`ENABLE_EXPORTS`), with its own libraries and the Cor-Libs whole-archived,
and `corPluginOpen` uses `RTLD_NOW`, so a plugin `.so` resolves `corLog`, `corAlloc`,
`corTree`, `corJson`, `corNgsild`, etc. from the running broker at `dlopen` time.

That holds for bridge plugins too. A bridge calls the broker's NGSI-LD side only
through the `BridgeBroker` slots, and may use the Cor-Libs directly: the loopback,
MQTT, Modbus and DDS bridges log with the `COR_*` macros and parse their
configuration with [corJson](https://github.com/SEAMWARE/corJson) and [corTree](https://github.com/SEAMWARE/corTree), in a corAlloc buffer of their own.
`BridgeBroker::logFunction` is for forwarding a transport library's own log sink.

A plugin's contract version (`BRIDGE_ABI_VERSION`, `TRANSPORT_ABI_VERSION`) covers
its structs and nothing else. A plugin that calls Cor-Lib functions must be built
against the **same** lib sources as the broker it will be loaded into — for a
packaged broker, the `coraine-dev` source of the same version
([Installation](installation.md#coraine-dev-build-your-own)). A function the broker
lacks fails the `dlopen`; a changed signature or struct layout is not detected - except the tree
node's, `CorNode`, which every plugin is checked for, and, for a DB or TRoE plugin, every header
its structs come from. Both below.

## The CorNode layout

Every kind of plugin reads and builds the broker's tree nodes (`CorNode`, corTree) by their fields,
so a plugin built against another node layout than its broker reads every node at the wrong
offsets. The layout has a number, `corNodeLayout` in `corTree/CorNode.h`: 2 since the prefix layout
(`value` last, a container's tail its last 8 bytes; layout 1, before it, had no number). Every object
built with that header carries it - a weak, exported symbol, so also a plugin built
`-fvisibility=hidden` - and every driver header (`DbDriver.h`, `TroeDriver.h`, `ApiPlugin.h`,
`AuthDriver.h`, `TransportDriver.h`, corBridge's `BridgeDriver.h`) includes it.

| Plugin | Broker | What happens |
|--------|--------|--------------|
| built against the broker's layout | any | loads |
| without `corNodeLayout`, or another number | this one | `corPluginOpen` refuses it before its register function: the broker prints why and exits 1 |
| built against layout 2 | from before layout 2 | the plugin requires the symbol `corNodeLayout2`, which only a corTree with layout 2 defines: the `dlopen` fails (`undefined symbol: corNodeLayout2`), the broker prints it and exits 1 |

A refusal, here a bridge built before layout 2:

```text
bridge plugin 'mqtt' (/opt/seamware/plugins/bridge/mqtt.so): refused - built against another tree-node layout than this broker (plugin: CorNode layout 1, or older - it has no corNodeLayout; broker: CorNode layout 2). It would read every node at the wrong offsets and crash the broker. Rebuild the plugin against this broker's Cor-Libs
```

The broker reads the plugin's own `corNodeLayout` - the one in the object its register function is
in, not one of its dependencies'. A change of the layout is a new number and a new required symbol
(`corNodeLayout3`), in `CorNode.h` and `CorNode.c`. Like the stamp below, the check is always
compiled in.

## The DB plugin interface stamp

A DB or TRoE plugin shares structs with the broker by layout: `DbDriver`, `DbQueryFilter`,
`Tenant`, `TroeDriver`, `CorNode`, ... A plugin built against other headers than its broker reads
and writes them at the wrong offsets and corrupts memory. So a mismatch is refused, never
tolerated - both ways, and the program exits 1:

| Side | Exports | Checks | Refuses |
|------|---------|--------|---------|
| broker (`coraine`, `coraine-import`) | `coraineDbAbi` | the plugin's `dbPluginAbi` (`dlsym` on the plugin), after the `dlopen`, before the register function | a plugin without `dbPluginAbi` (built before the check), or with another stamp |
| DB / TRoE plugin | `dbPluginAbi` | the broker's `coraineDbAbi` (`dlsym(RTLD_DEFAULT)`), first thing in `dbRegister` / `troeRegister` | a broker without `coraineDbAbi` (older than the plugin), or with another stamp |

The plugin's check is what stops a broker built before the check, which loads any plugin unchecked.
It exits rather than returning a failure: the register functions return nothing, and such a broker
would carry on with the plugin loaded.

The stamp is `tools/dbAbiStamp.sh`: a hash (sha256, 16 hex digits) of the headers the two sides
share structs through - `db/DbDriver.h`, `db/DbQueryFilter.h`, `db/Tenant.h`, `ha/HaEvent.h`,
`ha/haInit.h`, `troe/TroeDriver.h`, and every header they include with `#include "..."`, in this
repository's `src/lib` or in the Cor-Libs beside it (`tools/dbAbiStamp.sh --list` names them).
Comments are stripped (`gcc -fpreprocessed`) and whitespace collapsed before hashing: a comment
edit leaves the stamp as it was, any declaration change gives a new one. Nothing is bumped by hand.

The same script makes both sides' stamp: the broker's CMake writes it into
`<build>/generated/dbAbiStamp.h` on every build (rewritten only when it changed), and corDB's
makefile into `obj/<flavour>/dbAbiStamp.h`. The plugin side is `src/plugins/shared/dbPluginAbi.c`,
compiled into every DB and TRoE plugin - mongoc, none, timescale here; corDB.so, ramDB.so and
troe/ramDB.so in corDB.

A refusal names both stamps and the way out:

```text
DB plugin '/opt/seamware/plugins/db/currentState/corDB.so' does not match this broker: it has no DB plugin interface stamp; a plugin built against another DB plugin interface corrupts memory
  plugin's interface stamp: none - built before the check
  broker's interface stamp: 8fa2e0424436a91e
  rebuild the plugin against this broker's source, or install matching versions of the broker and its plugins
```

```text
DB plugin '/opt/seamware/plugins/db/currentState/corDB.so' refuses the broker '/usr/local/bin/coraine': the broker is older than the plugin - it has no DB plugin interface stamp; a plugin built against another DB plugin interface corrupts memory
  plugin's interface stamp: 8fa2e0424436a91e
  broker's interface stamp: none - built before the check
  rebuild the plugin against this broker's source, or install matching versions of the broker and its plugins
```

The check is always compiled in - it is a safety check of the plugin interface, not a feature,
and has no `COR_FEATURE_*` switch. API, bridge and transport plugins do not carry the stamp: a
bridge has `BRIDGE_ABI_VERSION` (below) and a transport `TRANSPORT_ABI_VERSION`
(`src/lib/plugin/TransportDriver.h`).

## Plugin-contributed CLI args

A plugin can publish its own command-line options. It sets `driverP->args`
(a `CorArg*` array) in its register function; the broker **peeks** at
`--database`/`--troe`/`--apiPlugins`/`--bridges`/`--auth` *before* the main parse, loads the
plugins,
then splices each plugin's `args` into the global arg table so they show up in
`--help` and parse normally. This is why `coraine --help` shows different options
depending on which DB/TRoE plugin you selected - and `coraine --auth fdsc -u` lists the
`--fdsc*` options under "Auth (fdsc) plugin options".

## NULL-allowed methods → graceful 501

Driver structs are big, and not every plugin implements every operation. The
convention: a **NULL function pointer means "unsupported"**, and the service
routine returns **501 Not Implemented** (or treats it as a no-op where the spec
allows). Examples called out in the headers: `subscriptionStatsFlush`,
`snapshot*`, `tenantDrop`, `tenantRelease`, and the whole context-persistence quartet
(`contextSave/Delete/List/Get`) are NULL on `corDB`. This is how the in-memory
driver legitimately ships without persistence.

## The driver interfaces

The contracts a plugin fills are fully documented (with per-function semantics) in
the headers — read these before writing a plugin:

- **`src/lib/db/DbDriver.h`** — current-state DB. Entity CRUD + bulk ops,
  subscriptions, registrations, snapshots, discovery (`typeList`/`attrList`),
  tenant setup, geo-match callbacks, and optional JSON-LD context persistence.
  Error codes: `DB_OK`, `DB_NOT_FOUND`, `DB_ALREADY_EXISTS`, `DB_INVALID_GEOMETRY`,
  `DB_BAD_INPUT`, `DB_ERR`.
- **`src/lib/troe/TroeDriver.h`** — temporal. The broker queues `TroeEvent`s
  during a request and drains them *after* the response (per-event or bulk
  `eventList`); read paths return `EntityTemporal` trees. Error codes: `TROE_OK`,
  `TROE_NOT_FOUND`, `TROE_UPDATED`, `TROE_ERR`.
- **`src/lib/plugin/AuthDriver.h`** — auth. `authenticate` (before anything is done
  for a request: 401), `authorize` (once the `@context` is resolved, before the
  tenant: 403), `release`; `AuthUnavailable` is a 503. Verdicts `AuthAllow`,
  `AuthUnauthenticated`, `AuthDeny`, `AuthUnavailable`.
- **`src/lib/plugin/ApiPlugin.h`** — extra endpoints. A flat
  `CorRestServiceSimplified[]` (verb + path + handler), optional URL `params`,
  optional `args`, and `init`/`close`/`versionInfo` hooks.
- **`corBridge`** — the bridge contract, three structs in three headers, at
  `BRIDGE_ABI_VERSION` **16**. Each slot below carries the revision that added it;
  the history comments in `BridgeBroker.h` and `BridgeDriver.h` are the reference.
  - **`BridgeDriver.h`** — what a bridge `.so` fills in (`BridgeDriver`):
    `alias`, `version`, `abiVersion`, `args`; ABI 1: `init`, `close`,
    `channelAdd`, `channelDel`, `publish`, `versionInfo`; ABI 2:
    `serviceInvoke`, `serverIface`; ABI 3: `serviceInvokeTracked`; ABI 4:
    `actionGoalSend`, `actionGoalCancel`; ABI 9: `channelAddInfo`; ABI 10:
    `notifySchemes`, `notify`; ABI 11: `serviceSchemes`, `serviceExecute`,
    `serviceCancel`; ABI 13: `payloadCheck`; ABI 14: `inventory`; ABI 15:
    `channelTypedAdd`. The broker does not call `channelDel` today: Channels come
    from `--bridgeConfig` and from discovered endpoints, and none is removed while
    the broker runs.
  - **`BridgeBroker.h`** — what the broker hands the plugin in `init()`
    (`BridgeBroker`): `abiVersion`; ABI 1: `sampleIn`, `logFunction`; ABI 2:
    `sampleQualifiedIn`; ABI 3: `replyIn`; ABI 4: `goalEventIn`; ABI 5:
    `goalEventPartIn`; ABI 6: `sampleMetaIn`, `replyMetaIn`, `goalEventMetaIn`;
    ABI 7: `replyExchangeIn`; ABI 8: `endpointDiscoveredIn`; ABI 11:
    `serviceUpdateIn`; ABI 12: `channelStatusIn`; ABI 16: `httpListen`.
  - **`BridgeServer.h`** — the peer side (`BridgeServer`, returned by
    `serverIface()`): `abiVersion`, `serviceServe`, `serviceUnserve`,
    `serviceReply`. The broker never uses it; the functional test client does.

  Error codes: `BRIDGE_OK`, `BRIDGE_NOT_FOUND`, `BRIDGE_UNSUPPORTED`,
  `BRIDGE_BAD_INPUT`, `BRIDGE_ERR`. These live outside the broker because the
  plugins are external. The structs are **append-only**; the broker writes its
  `BRIDGE_ABI_VERSION` into `BridgeDriver.abiVersion` before `bridgeRegister`, the
  plugin fills no slot the broker is too old to have and writes its own version
  back, and a mismatch is logged (INFO) rather than refused — an older plugin
  leaves the newer slots NULL, which is already how "unsupported" is spelled. A
  plugin checks `brokerP->abiVersion` and the pointer before calling a
  `BridgeBroker` slot added after ABI 1. `GET /version` shows each bridge's
  `versionInfo()` string under `bridges`; neither the broker's bridge ABI nor a
  plugin's `abiVersion` is in it.

## Bundled plugins

| Plugin | Category | Notes |
|--------|----------|-------|
| **mongoc** | DB | MongoDB via `libmongoc` v2; `$geoNear` aggregation, persistence, context hosting, per-tenant DBs. The default (`--database mongoc`). Needs the mongo-c **v2** driver at build time. |
| **corDB** | DB | In-process; GEOS geo-filtering, per-tenant isolation. In RAM, or on disk with `--dbDir` (a log and snapshots). With `--troe corDB`, its temporal history too. |
| **ramDB** | DB | corDB in RAM only: no disk options, no `--troe corDB` - the fastest store, for pub/sub where a restart may start empty. |
| **none** | TRoE | No-op. Temporal disabled. The default (`--troe none`). |
| **corDB** | TRoE | The temporal history inside the corDB store - `--troe corDB` takes it from the corDB current-state plugin and needs `--database corDB`; on disk with `--dbDir`. See [corDB's TRoE](https://github.com/SEAMWARE/corDB/blob/main/doc/troe.md). |
| **ramDB** | TRoE | A ring of the broker's TRoE events in RAM - the most recent N, nothing on disk, shown by `/admin/troe/dump` (the `admin` API plugin). Dev/test today: the functests assert the broker's TRoE event contract through it. |
| **timescale** | TRoE | TimescaleDB/Postgres-backed history (hypertables). |
| **admin** | API | `/admin/health`, `/admin/version`, `/admin/log` (GET/PUT/POST/PATCH/DELETE for verbose/info/traceLevels), `/admin/tenants`, `/admin/plugins`, `/admin/bridges/{bridge}/inventory` (what a bridge's source can carry, bridge ABI 14), `/admin/config` (every option, a secret masked), `/admin/probes` (what the code decided, for the functional tests). |
| **fdsc** | Auth | VCVerifier's access tokens (RS256/ES256 by `kid`, `iss`, `exp`) and the ODRL-PAP's policies, decided as the Data Space Connector 10.87.0 decides them. libcrypto. See [Authorization](authorization.md). |
| **oidc** | Auth | An OpenID provider's access tokens (Keycloak, ...; RS256/ES256 against its JWK Set) and two roles in them, one reading, one writing - decided per request, no policies. libcrypto. See [Authorization](authorization.md). |
| **ws** | Transport | WebSocket, upgraded from `GET /ngsi-ld/v1/ws` on the HTTP port (`--transports ws`). See [WebSocket](websocket.md). |
| **loopback** | Bridge | Not a transport: it hands back what it is given, **from a thread of its own**, which is the one property of a real bridge the broker has to survive. It makes an arriving value testable with no transport, publisher or network, and it is the reference a new bridge is written against — every entry point, one page, libc and pthreads. |
| **modbus** | Bridge | Modbus TCP registers and coils ↔ entity attributes: polled in, attribute writes out. Lives in [`corModbusBridge`](https://github.com/SEAMWARE/corModbusBridge), libc only, ships in the ordinary image, loaded only on `--bridges modbus`. See [The Modbus bridge](modbus-bridge.md). |
| **mqtt** | Bridge | MQTT, through libmosquitto. Delivers Subscription notifications to `mqtt://` and `mqtts://` endpoints (TS 104 243), which therefore need `--bridges mqtt` - without it such a Subscription is refused. Its Channels map MQTT topics to attributes, both ways. See [The MQTT bridge](mqtt-bridge.md). Lives in [`corMqttBridge`](https://github.com/SEAMWARE/corMqttBridge); the broker itself does not link libmosquitto. |
| **dds** | Bridge | DDS topics ↔ entity attributes, via eProsima's DDS Enabler. Lives in [`corDdsBridge`](https://github.com/SEAMWARE/corDdsBridge) and ships in the ordinary image, loaded only on `--bridges dds`. It links 21.4 MiB over nine libraries against the broker's own 4.28 MiB over three; an image that carries it and never loads it costs that disk space and nothing else. |
| **ckan** | Bridge | An open data portal's datasets (CKAN's API), discovered and read into entities through DataChannels. Lives in [`corCkanBridge`](https://github.com/SEAMWARE/corCkanBridge), no library, built by `coraine-build --bridges ckan`. |
| **opcua** | Bridge | An OPC UA server, through open62541: its Variables as Channels (monitored in, Writes out), its Objects as AssetChannels - a model browsed into a graph of entities. Sign / SignAndEncrypt with certificates and a trust list, a user and password. Lives in [`corOpcuaBridge`](https://github.com/SEAMWARE/corOpcuaBridge), built by `coraine-build --bridges opcua`. |

## Writing a new plugin (sketch)

A DB plugin is one `.so` exporting `void dbRegister(DbDriver*)`. Minimal shape,
mirroring `corDbRegister.c` in [corDB](https://github.com/SEAMWARE/corDB) - a DB plugin in a
repository of its own, built against this repo's `src/lib/db/DbDriver.h`:

```c
#include "db/DbDriver.h"

void dbRegister(DbDriver* driverP)
{
  driverP->alias          = "myStore";
  driverP->version        = "0.1.0";
  driverP->args           = myArgV;          // or NULL
  driverP->init           = myInit;          // post-arg-parse init
  driverP->close          = myClose;
  driverP->entityCreate   = myEntityCreate;
  driverP->entityRetrieve = myEntityRetrieve;
  driverP->entityQuery    = myEntityQuery;
  driverP->entityDelete   = myEntityDelete;
  // … fill what you support; leave the rest NULL (→ 501)
  driverP->tenantSetup    = myTenantSetup;
  driverP->tenantRelease  = myTenantRelease; // frees what tenantSetup hung on Tenant.pluginData
}
```

It must also carry the interface stamp: compile `src/plugins/shared/dbPluginAbi.c` into it (with
the generated `dbAbiStamp.h` on the include path) and call `dbPluginAbiBrokerCheck("DB")` first in
`dbRegister` - without them the broker refuses it.

Build it as a `SHARED` library that drops `myStore.so` into
`<base>/db/currentState/`, then run `coraine --database myStore`. The existing
plugin `CMakeLists.txt` files (e.g.
`src/plugins/currentState/mongoc/CMakeLists.txt`) are the template — note they
**don't** link the broker's libs (resolved at runtime), only their own backend
deps (`mongoc2`, `geos_c`, …). API and TRoE plugins follow the same pattern with
`apiRegister`/`troeRegister`.

---

Back to the [README](https://github.com/SEAMWARE/coraine#readme).
