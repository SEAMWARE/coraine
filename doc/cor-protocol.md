# The cor format and cor://

**cor** is coraine's binary serialisation of a tree - a request, a response, an entity - and
**cor://** is the broker's binary API: the same trees, framed for a long-lived TCP connection. A
request that arrives over cor:// is already a tree, so it goes into the same service routines an
HTTP request does, without a JSON parse, and its response goes back as a tree.

The full specification - the encoding, the framing, multiplexing, the numbers - is in
[the details](cor-protocol-details.md). How it came about: [history](history/cor-protocol.md).

## When to use it

- **Between brokers.** A registration whose endpoint is `cor://host:port` makes forwarding to that
  context source go over cor:// instead of HTTP: no JSON rendered or parsed on either side of the hop.
- **A client that speaks it.** [corTools](https://github.com/SEAMWARE-cor/corTools)' `corRequest` sends requests over cor://, one at a time or in
  its load mode.
- **Not** for what only HTTP carries: a body that is not JSON, a text answer, HEAD.

cor:// is a communication protocol, not a bridge: it reaches the broker's API, both as a client
(forwarding) and as a server (the listener).

## How to turn it on

| | |
|---|---|
| the listener | `--corPort <port>` - a cor:// listener next to the HTTP one (0, the default: off) |
| forwarding | a context source registration with `"endpoint": "cor://host:port"` |
| the functests | `COR_TRANSPORT=cor corTest ...` - every test that can runs over cor:// (each broker on its HTTP port + 1000); `doc/testing.md` |
| the benchmark | `test/perf/corChain.sh <http\|cor\|cor-all>` - three brokers chained |

**Compiled out**: `make COR_WITH_COR_PROTOCOL=0` (or `coraine-build --features COR_PROTOCOL=OFF`) builds a
broker without cor:// - [corRest](https://github.com/SEAMWARE-cor/corRest) without its cor:// server and client (`corRestCor.c`), the broker without
`--corPort` and without the cor:// forwarder: 24 640 bytes less (stripped, 2026-10-13). `--version` and
`GET /build` say `COR_PROTOCOL` 0 / false. A registration whose endpoint is cor:// is accepted and not
forwarded to: the answer carries `NGSILD-Warning: 299 <host:port> "The registration endpoint's scheme is not
supported by this broker"` (`cor_not_in_this_build.test`). The cor format stays: [corDB](https://github.com/SEAMWARE-cor/corDB)'s log and history are
written in it.

## What it is

- **One format, three uses**: cor://, and the bodies of the corDB log and snapshot records.
- **The codec knows nothing about NGSI-LD.** It lives in [corTree](https://github.com/SEAMWARE-cor/corTree) and serialises any tree; [corNgsild](https://github.com/SEAMWARE-cor/corNgsild)
  supplies, through callbacks, which names are core terms and which objects are attributes.
- **Zero-copy on read** - strings keep their NUL and point into the read buffer; **skippable** -
  objects and arrays carry their byte length; **little-endian**, no byte swapping.
- **Small**: core terms by id, an attribute's type folded into its node, IRIs split into a namespace
  and a local name, a string table - and on cor:// the tables live as long as the connection.
- **Every frame** starts with the magic `C0 4F 52 01`, which no text or HTTP request can begin with:
  a wrong peer is refused at once.
- **Multiplexed**: any number of requests in flight on one connection, answered in the order they
  finish, run on the event loops' coroutines (`doc/coroutines.md`). A distributed operation fanned
  out to several cor:// sources has them all in flight at once. With `mongoc` a request that waits is
  served on a thread of its own, one at a time.

## Key numbers

Three `corDB` brokers chained A -> B -> C, a GET on A, 16 connections; each broker on two cores,
built-in server, release build with PGO, deep idle states, two runs (2026-11-19,
[details § 6](cor-protocol-details.md#6-what-is-built-and-what-it-measures)):

| Entity | Mode | req/s | p99 |
|---|---|---:|---:|
| 4 attributes | HTTP all the way | 40,718-40,934 | 1.44-2.19 ms |
| 4 attributes | cor:// between the brokers, HTTP client | 76,319-76,453 | 415-429 us |
| 4 attributes | cor:// all the way | 86,930-90,067 | 242-253 us |
| 20 attributes | HTTP all the way | 24,720-25,376 | 1.27-1.30 ms |
| 20 attributes | cor:// between the brokers, HTTP client | 35,200-40,809 | 0.90-1.90 ms |
| 20 attributes | cor:// all the way | 46,813-48,306 | 424-446 us |

One broker, no forwarding: cor:// and HTTP are at parity on throughput (~145,000 against ~142,000
req/s with 16 connections), the p99 under load 209 us against 4-9 ms. On the wire, with the NGSI-LD
callbacks, 71 % of minimised JSON over 660 test payloads.
