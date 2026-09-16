# Credits

coraine is developed and maintained by **Seamware**.

- **Ken Zangelin** — author and maintainer.

## Standing on

The list below is short on purpose. coraine implements its own HTTP server, JSON
parser, JSON-LD processor, NGSI-LD engine, memory allocator, argument parser,
hash tables, metrics and test harness — the k-libs and the Cor-Libs named after
the table, all of them in this project's own repositories and all of them inside
the ~1 MiB binary. What it borrows is work that is genuinely someone else's
speciality: TLS, computational geometry, Unicode collation, and the client
libraries of two database servers. None of it is NGSI-LD, and most of it is
optional.

| Project | Used for | Needed when |
|---------|----------|-------------|
| [OpenSSL](https://www.openssl.org/) | TLS | always |
| [libmosquitto](https://mosquitto.org/) | MQTT notifications | always |
| [GEOS](https://libgeos.org/) | geo-query evaluation | with either current-state plugin |
| [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) | the HTTP server the REST layer sits on | only `COR_HTTP_SERVER=mhd` — `corHttp`, ours, is the alternative |
| [ICU](https://icu.unicode.org/) | `orderBy` string collation (§ 7.6.2.1) | only `COR_FEATURE_ICU_COLLATION=ON` |
| [MongoDB C driver](https://github.com/mongodb/mongo-c-driver) (v2) | the `mongoc` current-state plugin | only `--database mongoc` |
| [PostgreSQL / libpq](https://www.postgresql.org/) | the `timescale` temporal plugin | only `--troe timescale` |
| [TimescaleDB](https://www.timescale.com/) | temporal storage (hypertables) | only `--troe timescale` |

The last five are loaded only if you ask for them: a `corHttp` + `corDB` broker
maps three libraries beyond what a bare `ubuntu:26.04` already has, and two of
those three are GEOS.

The **k-libs** (`kbase`, `kalloc`, `khash`, `kjson`, `kargs`, `ktrace`,
`kprom`) and the **Cor-Libs** (`corRest`, `corNgsild`, `corJsonld`, `corPlugin`,
`corTest`) are ours, developed alongside the broker and released separately.

## The specification

coraine implements **NGSI-LD**, and two bodies deserve the credit for it.

The version implemented here, **ETSI GS CIM 009 v1.9.1**, was written by the ETSI
Industry Specification Group on cross-cutting Context Information Management
(**ISG CIM**) — the group that created NGSI-LD and carried it through every version
up to that one. It is named without a link on purpose: its committee page,
`etsi.org/committee/cim`, now returns 404.

NGSI-LD is now developed in
**[ETSI Technical Committee Data Solutions (TC DATA)](https://www.etsi.org/technical-groups/data/)**,
which is where the specification continues and where its current numbering comes
from: **TS 104 175** (core API), TS 104 176 (HTTP binding) and TS 104 243 (MQTT
notification binding). Their foreword says it plainly — *"This Technical
Specification (TS) has been produced by ETSI Technical Committee Data Solutions
(TC DATA)"*. ISG CIM still formally exists, but it is winding down; new NGSI-LD work
happens in TC DATA.

Conformance is measured against the official
[ETSI NGSI-LD test suite](https://forge.etsi.org/rep/cim/ngsi-ld-test-suite).

## FIWARE

This project is part of [FIWARE](https://www.fiware.org/).
