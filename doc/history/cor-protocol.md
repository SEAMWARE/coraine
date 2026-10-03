# The cor format and cor:// - history

How cor:// came about: the plan v1 was built to, what v1 measured, and multiplexing tried on threads
before it was built on the coroutines. What cor:// is today is in [The cor format and
cor://](../cor-protocol.md). A bare section number (§ 4.13) is one of
[the details](../cor-protocol-details.md). Newest first.

## 2026-10-02 - cor:// multiplexing, on threads and then on the coroutines

### 5.3 Multiplexing - on the coroutines (2026-10-02)

Many requests in flight on one connection, answered in the order they finish: no byte of the format
changed - the frames carry correlation ids from the start.

**The one rule** any design has to keep: the tables follow the stream (§ 4.13) - frames are encoded
in the order they are sent and decoded in the order they arrive.

**Built on threads first, and dropped.** Every threaded design put a thread hand-off on a request's
path, and on two cores per broker CPU per request is the throughput: the best of them cost 5-11 % of
the three-broker chain (16 callers, `perf stat`, release builds):

| server | client | req/s | thread switches per request (broker A) |
|---|---|---:|---:|
| a thread per connection, one request in flight | a connection per calling thread | 74 000 | 1.6 |
| requests that wait handed to the worker pool | 2 shared connections per peer | 48 000 | 3.2 |
| same | an idle connection first, up to 16 | 57 000 | 2.7 |
| a thread owns the connection, the event loop takes what arrives meanwhile | same | 66 000 | 2.0 |

**Built on the coroutines** (`doc/history/coroutines.md` § 11), where many requests in flight on one thread is
simply what the event loop does:

- **server**: a connection is armed once, for good; the loop reads every frame that comes, decodes
  each request as it arrives (wire order), runs it - inline, or as a coroutine - and queues each
  response, encoded as it is queued (wire order again), flushed on `EPOLLOUT` when the socket is full.
  A request in flight holds a reference to the connection - one may finish on a worker.
- **client**: one connection a peer, per thread, shared by every request of the thread. A frame goes
  out under the connection's *write turn* (a coroutine that finds it taken parks); the first request
  that waits and finds no reader takes the *read turn*, decodes every frame as it comes into the memory
  of the call it belongs to, wakes that call's coroutine (corBase `corCoLoopPark/Wake`), and hands the
  turn on when its own response is in. A response nobody waits for any more is decoded all the same -
  the tables need it - and dropped.
- `corRestCorStart` / `corRestCorWait`: a request sent now, its response collected later. A
  distributed query fanned out to several cor:// sources starts every request before it waits for any.
- With `mongoc` a request that waits cannot be a coroutine (the driver blocks): its connection moves
  to a thread of its own, one request at a time, as before.

Two functests fail without it: `cor_multiplex_out_of_order` (a fast request answered before a slow one
sent earlier on the same connection) and `cor_fanout_concurrent` (two sources of one second each: two
seconds before, one now). Measured in `doc/performance.md` - no loss anywhere, the instructions per
request unchanged.


## 2026-10-01 - cor:// v1: the plan it was built to

### 6. Order of work

1. **The codec** in corTree, with the corNgsild callbacks (§ 4.1-4.3), round-trip exact.
2. **cor:// forwarding**, client and server in corRest, proven on a **chain of three brokers**:
   A and B each hold a registration pointing at the next, C holds the entity. A query to A is
   forwarded to B, then to C, and the entity comes back all the way. The same chain is run twice,
   once with `http://` registrations and once with `cor://`:
   - the answers must be identical, byte for byte - the functest
   - the difference in latency and throughput is the benchmark, and the § 4.12 sizes are measured
     on the same traffic
3. **The corDB snapshot** - the same codec to a file, and back on start.
4. **The corDB log** - the same records appended, replayed on start; history by retention.

### 6.1 v1, measured (2026-10-01)

Steps 1 and 2 are built: the codec (corTree), its NGSI-LD callbacks (corNgsild), cor:// in corRest -
a listener (`--corPort`) and a client - and forwarding over it. `cor_forwarding_chain` and
`cor_api_direct` are the functests; `test/perf/corChain.sh` the benchmark: three corDB brokers chained
A -> B -> C, a GET on A, so every request crosses both hops twice. Release build, each broker on two
cores, two runs each (ranges):

| Entity | Mode | 1 conn req/s | p50 | p99 | 16 conns req/s | p50 | p99 |
|---|---|---|---|---|---|---|---|
| 4 attributes | http | 6,519-7,039 | 146 us | 1.1-1.2 ms | 23,515-23,595 | 624 us | 1.74 ms |
| 4 attributes | cor (hops) | 8,428-8,585 | 111 us | 0.2-0.7 ms | 40,912-44,887 | 337-362 us | 0.84-1.03 ms |
| 4 attributes | **cor-all** | **14,293-14,793** | **66 us** | **78-94 us** | **64,056-65,012** | **240-245 us** | **399-401 us** |
| 20 attributes | http | 3,848-4,030 | 250 us | 0.3-1.6 ms | 17,261-17,556 | 0.86 ms | 1.98-2.11 ms |
| 20 attributes | cor (hops) | 4,915-4,944 | 192 us | 1.4-2.3 ms | 28,233-28,310 | 537 us | 1.18-1.19 ms |
| 20 attributes | **cor-all** | **6,347-6,669** | **162 us** | **183-190 us** | **39,697-40,138** | **391-395 us** | **642-647 us** |

- **http** - every hop HTTP and JSON; the client is wrk
- **cor (hops)** - the two forwarding hops cor://, the client still wrk over HTTP
- **cor-all** - nothing but cor://: the client is `corRequest` in its load mode

All cor:// against all HTTP: **2.1-2.2x with one connection, 2.3-2.8x with sixteen**, p99 under load
4x lower for the small entity and 3x for the large; the single-connection p99 stops being noise
(78-190 us, where every run with an HTTP client leg swings between 0.2 and 2.3 ms).

**One broker, no forwarding** - what the transport itself costs, a GET on corDB:

| | 1 conn req/s | p99 | 16 conns req/s | p99 |
|---|---|---|---|---|
| HTTP (wrk) | ~41,000 | ~33 us | ~142,000 | 4-9 ms |
| cor:// (corRequest) | 43,400 | 31 us | 145,300 | 209 us |

At parity on throughput, with a tail an order of magnitude tighter under load. (The first cut was
well behind - 29,000 / 82,000 req/s: it polled before every read and write and read and sent the
header and the tree separately. Buffered reads and one send per frame took it from ~8 system calls a
request to ~2.)

Sizes on the wire: the generic codec gives 84 % of minimised JSON, with the NGSI-LD callbacks 71 %,
over 660 JSON documents of the ETSI suite and coraine's tests - payloads with compact names and
inline `@context` text, so the least favourable case; broker-to-broker traffic, with its expanded
names, is to be measured on the chain.

Since 2026-10-02 a request that waits runs as a coroutine of the server's event loop (`doc/history/coroutines.md`
§ 8): the chain end to end with 16 callers went from ~68 000 to ~80 000 req/s, its p99 from ~380 to
~225 µs.

Not yet: multiplexing (§ 5.3), and with it a fan-out to several cor:// sources that has them all in
flight at once; packed numeric arrays and timestamps as integers (§ 4.9, § 4.10).


## Before 2026-10-01 - the TLV drafts

The cor format supersedes the earlier TLV drafts, whose type codes carried NGSI-LD meaning. Here the
format carries *trees*, and the meaning comes from the core-term ids NGSI-LD already has.
