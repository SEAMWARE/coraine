# Credits

coraine is developed and maintained by **Seamware**.

- **Ken Zangelin** — author and maintainer.

## Standing on

coraine is a small program because other people wrote the hard parts first. It links,
at build or at run time:

| Project | Used for |
|---------|----------|
| [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) | the HTTP server the REST layer sits on |
| [MongoDB C driver](https://github.com/mongodb/mongo-c-driver) (v2) | the `mongoc` current-state plugin |
| [GEOS](https://libgeos.org/) | geo-query evaluation in memory |
| [libmosquitto](https://mosquitto.org/) | MQTT notifications |
| [OpenSSL](https://www.openssl.org/) | TLS |
| [ICU](https://icu.unicode.org/) | `orderBy` string collation |
| [PostgreSQL / libpq](https://www.postgresql.org/) | the `timescale` temporal plugin |
| [TimescaleDB](https://www.timescale.com/) | temporal storage (hypertables) |

The **k-libs** (`kbase`, `kalloc`, `klog`, `khash`, `kjson`, `kargs`, `ktrace`,
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
