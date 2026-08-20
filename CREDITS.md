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

coraine implements **ETSI GS CIM 009 / TS 104 175 — NGSI-LD**, written by the
[ETSI Industry Specification Group on cross-cutting Context Information Management
(ISG CIM)](https://www.etsi.org/committee/cim). Conformance is measured against the
official [ETSI NGSI-LD test suite](https://forge.etsi.org/rep/cim/ngsi-ld-test-suite).

## FIWARE

This project is part of [FIWARE](https://www.fiware.org/).
