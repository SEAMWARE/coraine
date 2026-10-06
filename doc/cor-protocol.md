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
- **A client that speaks it.** corTools' `corRequest` sends requests over cor://, one at a time or in
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

## What it is

- **One format, three uses**: cor://, and the bodies of the corDB log and snapshot records.
- **The codec knows nothing about NGSI-LD.** It lives in corTree and serialises any tree; corNgsild
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
built-in server, release build with PGO, deep idle states, two runs (2026-10-02,
[details § 6](cor-protocol-details.md#6-what-is-built-and-what-it-measures)):

| Entity | Mode | req/s | p99 |
|---|---|---:|---:|
| 4 attributes | HTTP all the way | 42,992-44,418 | 0.85-2.23 ms |
| 4 attributes | cor:// between the brokers, HTTP client | 79,991-80,764 | 333-346 us |
| 4 attributes | cor:// all the way | 91,490-91,945 | 235-237 us |
| 20 attributes | HTTP all the way | 24,415-25,127 | 1.26-1.30 ms |
| 20 attributes | cor:// between the brokers, HTTP client | 39,703-40,871 | 784-810 us |
| 20 attributes | cor:// all the way | 50,988-53,642 | 398-402 us |

One broker, no forwarding: cor:// and HTTP are at parity on throughput (~145,000 against ~142,000
req/s with 16 connections), the p99 under load 209 us against 4-9 ms. On the wire, with the NGSI-LD
callbacks, 71 % of minimised JSON over 660 test payloads.
