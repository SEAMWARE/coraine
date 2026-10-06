# The cor format and the cor:// protocol - in detail

The full specification: the encoding of a tree, the tricks that make it small, the cor:// framing
and multiplexing, what is built and what it measures. The short version - what cor:// is and how to
use it - is [The cor format and cor://](cor-protocol.md); how it came about is in its
[history](history/cor-protocol.md).

## 1. What it is for

One binary serialisation of a `CorNode` tree, used in three places:

| Consumer | What it writes |
|---|---|
| corDB **snapshot** | every entity of a tenant, to a file, and back on start |
| corDB **log** | each write as a record, appended - durability, and history (TRoE) by retention |
| **cor://** | the same trees, framed for a socket: requests forwarded between brokers, the edge, and later a standalone corDB |

Designing it once for all three is the point. Two serialisers - one for the disk, one for the wire -
would diverge, and the second always arrives as "the other format".

**Goals, in order:** no JSON parse on the hot path; zero-copy on read (strings point into the
buffer); small; skippable (a reader can step over a subtree it does not want without walking it);
evolvable without breaking old readers.

## 2. Two layers

```
  cor://   framing: magic, version, message type, correlation id, length      (§ 5)
  ──────────────────────────────────────────────────────────────────────────────
  format   one encoded tree per message / record                              (§ 3, § 4)
```

**The codec lives in corTree and knows nothing about NGSI-LD.** It serialises any tree. What it
cannot know - which names are core terms, which objects are attributes - is supplied by corNgsild
through callbacks, the same way corJson takes its term-intern callback. The same codec could carry
any JSON.

## 3. Layer 1 - a node on the wire

```
  tag     1 byte    value type (3 bits) | name mode (2 bits) | flags (3 bits)
  [kind]  1 byte    objects only: the 4-bit kind, + 4 spare bits                     (§ 4.2)
  name              per name mode: nothing | a term id | a string                   (§ 4.1, § 4.4)
  value             per value type
```

| Value type | Value on the wire |
|---|---|
| string | a string (§ 4.4) - or a term id when the value is a core term (§ 4.3) |
| int | zigzag varint - or 0-6 in the tag itself (§ 4.7) |
| float | 8 bytes, or 4 when lossless (§ 4.8) |
| boolean, null | nothing - the value is in the tag |
| object, array | byte length (varint), then the children |

| Name mode | Name on the wire |
|---|---|
| 0 | none - an array element |
| 1 | a core term id (§ 4.1) |
| 2 | a string (§ 4.4) |
| 3 | a back-reference into the message's string table (§ 4.5) |

**Byte length, not child count, for objects and arrays.** It is what makes a subtree skippable:
a reader that wants only `speed` steps over every other attribute in one add. The count is
derivable while reading; the length is not.

**Strings keep their NUL** on the wire and on disk, so a decoded `CorNode` points straight into the
read buffer: zero-copy, and no allocation per string. The cost is one byte per string.

**Little-endian.** Both ends are ours, and x86 and ARM are both little-endian: no byte swapping,
anywhere.

## 4. Making it small

Each trick below is independent: it adds a tag bit or a mode, and a reader that does not use it
still reads everything else. **v1** marks the ones worth having from the start.

### 4.1 Core terms by id - v1

A member whose name is a core term (`type`, `value`, `observedAt`, `object`, `unitCode`, ...)
carries its `CorTerm` id instead of the name. The ids are append-only and already documented as
"sent on the binary wire".

They are written as **one byte, with 0xFF as an escape to two**, not as a varint. The core context
has about 230 terms, so every id fits in one byte today. A varint would spend two on the most
common ones (`value` is about 204, `observedAt` about 138).

### 4.2 The attribute's type in the node - v1

An attribute's `type` is one of 8 values (`Property`, `Relationship`, `GeoProperty`,
`LanguageProperty`, `VocabProperty`, `JsonProperty`, `ListProperty`, `ListRelationship`). With
"none", that is 9, which fits in 4 bits. So an attribute is **one node that says what it is**:

```
JSON:   "speed": { "type": "Property", "value": 42, "observedAt": "..." }
wire:   [object, kind=Property] speed { value: 42, observedAt: ... }      - no "type" member
```

Every attribute loses a whole child node. The reader knows the type from one byte, with no
lookup and no string compare - today `ldAttrTypeDetect` finds and compares the member.

**corTree only carries the nibble**, 0 meaning "a plain object". corNgsild fills it on encode,
folding the `type` member away, and expands it on decode. Values 9-15 stay free for other
well-known objects (an entity; an entity's `@context`) without a format change.

### 4.3 Core terms as values - v1

A string *value* that is a core term travels as its id too: an attribute's `type` where it is not
folded (§ 4.2), `@type: DateTime`, the vocab values that are core terms. Flag bit in the tag.

### 4.4 Namespaces - v1

Nearly every long string in NGSI-LD is an IRI, and IRIs share long beginnings: every user term of a
context expands under that context's own namespace (`https://smartdatamodels.org/dataModel.Transportation/`
+ `speed`), every term without one under the default context, and most entity ids under
`urn:ngsi-ld:` + the type.

So an IRI is written **split at its last `/`, `#` or `:`** into a *namespace* and a *local name*:

```
namespace   an index into the message's namespace table (1 byte, varint beyond 127)
local name  inline: speed
```

- **The table is built as the message is written.** A namespace's first occurrence is written in
  full and numbered; every later one is its index. Any context's prefix is compressed, with nothing
  to know or configure about the context - a user context with 40 terms under one namespace pays
  for that namespace once.
- **It does not start empty.** The NGSI-LD namespaces are its first, fixed entries:
  `https://uri.etsi.org/ngsi-ld/default-context/`, `https://uri.etsi.org/ngsi-ld/`, `urn:ngsi-ld:`.
  The fixed part is append-only and its length is exchanged in HELLO (§ 5).
- **On cor:// the table lives as long as the connection** - which is the point of a persistent
  connection. A namespace is sent once and then reused by every request and response after it, so
  after the first few messages a whole context's prefixes cost a byte each. See § 4.13 for the rules
  that makes safe.
- **A snapshot or a log block keeps its own table**, so a block can still be read on its own.

A user attribute name under the default context - 45 bytes of namespace plus `speed` - becomes
7 bytes; under a user context, the same after its first occurrence.

### 4.5 A string table - v1 for names, later for values

The same strings repeat within a message: a query result of 100 vehicles has 100 times `speed`,
`location` and the `Vehicle` type IRI. The first occurrence is written in full and implicitly
numbered; every later one is name mode 3, a varint index. This costs nothing when there are no
repeats, so a single-entity message is no larger.

On a cor:// connection the table lives as long as the connection, like the namespace table (§ 4.13);
in a log segment or a snapshot it is per block, so a block is still readable on its own.

### 4.6 Front coding of ids - later

Entity ids in a result share long beginnings (`urn:ngsi-ld:Vehicle:B-0001`, `...B-0002`). A string
can be "the first *n* bytes of the previous string of this kind + the rest". This is cheap to
write and read, and the largest remaining win on big query results.

### 4.7 Small values in the tag - v1

Booleans and null need no value bytes. An integer 0-6 (counts, enum-like values, small readings)
lives in the tag's three flag bits - 7 there meaning "a varint follows" - so the whole value is one
byte.

### 4.8 Numbers by their real size - v1

- integers: a zigzag varint - 42 is one byte
- a float that is lossless as a float32: 4 bytes, not 8
- the JSON distinction between `42` and `42.0` is kept, because the value type says which

### 4.9 Packed numeric arrays - v1

An array whose elements are all numbers is written as one element type, a count and the raw values,
with no tag per element. GeoJSON coordinates are exactly this, and they are most of a
`GeoProperty`'s bytes: a 100-point polygon goes from 100 x (tag + 8) to 100 x 8 + 3 bytes, or x 4
where float32 is lossless.

### 4.10 Timestamps as integers - v1, with a guard

`observedAt`, `createdAt`, `modifiedAt` and every `DateTime` value are 24-byte strings. As
microseconds since the epoch they are a 7-8 byte varint. An `observedAt` is the user's and comes
back exactly as it was given, so the encoder converts only when the canonical rendering of the
integer reproduces the string byte for byte. Otherwise it stays a string. Nothing is ever
normalised in transit.

### 4.10a System timestamps - one per created entity

`createdAt` and `modifiedAt` are not the user's, and they are already integers: corNgsild attaches
them on write (`ldApiEntityToDbModel`, from the request's one time) and renders them on read
(`ldEntityToApi`, only when a request asks - `options=sysAttrs`, a temporal
`timeproperty=modifiedAt`). The broker owns the clock: one time per request, so the current state,
TRoE and the notifications agree to the microsecond.

An entity is created whole, with one time. corDB and mongoc store that one time - the entity's
`createdAt` - and, below the entity, only the times that differ from it (corDB's README):

| | stored |
|---|---|
| a created entity | the entity's `createdAt`; its `modifiedAt` once it differs |
| a modified attribute | its own `modifiedAt` - its `createdAt` is still the entity's |
| an added attribute | its own `createdAt` and `modifiedAt` |

A time that is not there is the entity's `createdAt`; a store with every time in place is read as it
is. The conversion is at each store's edges - every tree that leaves the store has every timestamp - so
**cor://** is unchanged, and corNgsild knows one thing: its in-place attribute update
(`ldEntityAttrsSet`) keeps an inherited `createdAt` inherited.

- **corDB**: 100k entities of ten attributes take 21-30 % less memory (the figures, and what it does to
  throughput: corDB's README).
- **mongoc**: in the conversion that already walks the document (`mongocEntityToBson` /
  `mongocAttrAppend` out, `mongocEntityBsonToTree` in - no extra copy). A `q` on an inherited time -
  the entity's `modifiedAt`, an attribute's or a sub-attribute's `createdAt` / `modifiedAt` - is
  `{ $or: [ { <time>: <cmp> }, { <time>: absent, <its object>: there, createdAt: <cmp> } ] }`; the
  entity's `createdAt`, always there, keeps its plain compare and its `{createdAt, _id}` index. An
  entity's document (`bsonsize`, ten Property attributes): 1,603 -> 1,193 bytes (**-25.6 %**); with
  two sub-attributes and an `observedAt` per attribute: 4,433 -> 3,243 bytes (**-26.8 %**).

  Throughput (2026-10-07): `test/perf/perfRun.sh`, release builds, one broker binary with the two
  `mongoc.so` (main / one time per entity), broker on 8 cores; the mean of four runs each, run in the
  order M B M B B M B M; requests/s:

  | scenario | every time kept | one per entity | | runs, every time kept | runs, one per entity |
  |---|---:|---:|---:|---|---|
  | query_c50 | 17,384 | 16,300 | **-6.2 %** | 16,840..18,814 | 15,103..16,760 |
  | query_c200 | 16,686 | 16,162 | **-3.1 %** | 16,208..17,815 | 16,025..16,255 |
  | query_l1_c50 | 27,245 | 27,311 | **+0.2 %** | 26,505..27,822 | 27,123..27,418 |
  | query_l100_c50 | 6,056 | 6,080 | **+0.4 %** | 5,934..6,150 | 6,035..6,105 |
  | retrieve_c50 | 68,663 | 69,068 | **+0.6 %** | 66,021..70,506 | 68,558..69,436 |
  | patch_c50 | 19,770 | 19,625 | **-0.7 %** | 19,340..20,180 | 19,015..19,920 |
  | patch_c1 | 3,460 | 3,447 | **-0.4 %** | 3,414..3,516 | 3,415..3,496 |
  | batch20_c50 | 1,568 | 1,589 | **+1.4 %** | 1,542..1,587 | 1,578..1,594 |
  | create_c50 | 54,555 | 55,768 | **+2.2 %** | 52,205..55,682 | 55,459..56,105 |
  | create_c1 | 7,011 | 7,035 | **+0.3 %** | 6,862..7,205 | 6,923..7,136 |
  | batch20create_c50 | 6,212 | 6,376 | **+2.6 %** | 6,004..6,467 | 6,276..6,541 |
  | merge_c50 | 24,369 | 24,424 | **+0.2 %** | 23,720..24,766 | 23,695..24,741 |
  | delete_c50 | 29,926 | 30,231 | **+1.0 %** | 29,021..30,291 | 30,071..30,376 |
  | batch20delete_c50 | 3,171 | 3,189 | **+0.6 %** | 3,122..3,202 | 3,179..3,198 |

  query_c50 and query_c200 are the first two measurements after perfRun's fixture. The same request
  (`limit=20`, 50 connections) measured alone - the same fixture, 10 s warm-up, 3 x 5 s, the two
  alternated three times each: 16,018 against 15,975 requests/s (**-0.3 %**, inside the 16,298..15,851
  drift of the whole series); limit=1 and limit=100 above, the same read path, are +0.2 % and +0.4 %. cor:// carries the times
only when the request asked for them, as integers - no guard needed, § 4.10's "convert only if it
re-renders exactly" is for the user's `observedAt`.

### 4.11 Compression of a whole frame - later, off by default

A flag bit in the frame header for zstd. Pointless on a LAN after § 4.1-4.10; possibly worth it over
a WAN link to an edge node. It costs zero-copy for that frame.

### 4.12 An estimate - to be measured, not quoted

One entity with one attribute, as the broker holds it (expanded names, system timestamps), is about
230 bytes of JSON. With § 4.1-4.10 it is roughly 60-70 bytes. A 100-entity query result gains
further from § 4.5, because after the first entity every repeated name and type is one or two
bytes. **The first implementation measures this on the functests' real entities**, and this
section is then rewritten with those numbers.

### 4.13 Tables that outlive a message - cor:// - v1

The namespace table (§ 4.4) and the string table (§ 4.5) are **per connection**: what one message
defined, every later one on the same connection may reference. The rules that make it safe:

- **One table set per direction.** Each side writes to its own outgoing tables, and keeps a mirror
  of what the peer has defined. Neither side ever writes to the other's.
- **Decoding in stream order.** A message may reference an entry an earlier message defined, so the
  connection's reader applies each message's table additions in arrival order before handing the
  request to a worker. Responses may still complete out of order - only the table bookkeeping is
  sequential, and TCP already delivers in order.
- **A connection starts empty**, apart from the fixed entries, and a reconnect starts empty again,
  on both sides.
- **A cap.** A busy connection lives for days. When a table reaches its limit the sender adds
  nothing more and writes new strings inline, or sends a RESET that empties both copies. Either way
  the reader always knows the table's exact state.

## 5. Layer 2 - cor:// framing

Every message: a fixed 16-byte header, then one encoded tree (§ 3).

| Field | Bytes | |
|---|---|---|
| magic | 4 | `C0 4F 52 01`: 0xC0, `O`, `R`, format version 1 (§ 5.1) |
| message type | 1 | HELLO, HELLO_ACK, REQUEST, RESPONSE, PUSH, CLOSE, ERROR, PING, PONG |
| flags | 1 | bit 0: compressed (§ 4.11); bit 1: a fragment, more follows |
| reserved | 2 | zero |
| correlation id | 4 | a REQUEST's, copied into its RESPONSE; 0 for PUSH |
| length | 4 | the body's |

### 5.1 The magic

The first four bytes of every cor:// frame: **`C0 4F 52 01`** - 0xC0 (which a hex dump shows as `C0`:
almost "CO"), then `O`, `R`, and the format version. corDB's log and snapshot records begin with their
own four bytes, `c` `r`, the record version and the op (corDB's `doc/persistence.md` § 4).

- **Wrong peer, said at once.** An HTTP client on the cor:// port, or the reverse, shows in the first
  bytes - `GET ` is not `C0 4F 52` - and the connection is refused with a clear error, not a
  confusing length.
- **Never text.** 0xC0 can never appear in valid UTF-8 at all (it could only begin an overlong
  encoding, which UTF-8 forbids), so no text and no HTTP request can start with the magic. (PNG's
  `0x89` is the same idea.)
- **Resynchronisation.** After a framing error the reader can scan for the next magic.
- **Recognisable.** In a hex dump or a capture - and it carries the format version before HELLO has
  been read.

### 5.2 The bodies

**The bodies are trees, not codes.** A request is `{ op, tenant, path, params, body }`; a response
is `{ status, headers, body }`, the body being an entity, a list or a problem details. A new
operation is a new value of `op`, not a new type code, and an old peer answers it with "not
implemented" instead of failing to parse it.

**HELLO** carries the format version, and the lengths of the fixed namespace entries (§ 4.4) and
of the term table (§ 4.1). Both tables are append-only, so two peers simply use the shorter of each, and a newer peer
writes a term the older one lacks as a string.

**One long-lived TCP connection per peer**, both sides able to send REQUESTs, responses out of
order by correlation id, PUSH for notifications, PING/PONG when idle, reconnection with back-off.
TLS underneath when the link leaves the host.

**cor:// is a communication protocol, not a bridge.** The plugin architecture keeps the two apart:
a communication protocol is how a client - or another broker - reaches the broker's API; a bridge
is how the broker reaches a foreign wire, and a bridge never answers requests. cor:// has to do
both halves of the API:

- **the client side** - a registration whose endpoint is `cor://host:port` makes forwarding encode
  the request as a tree, send it and decode the response tree, where `http://` goes through the
  HTTP client as today
- **the server side** - a cor:// listener next to the HTTP one: a request frame is already a tree,
  so it goes into the same service routines an HTTP request does, without a JSON parse, and the
  response goes back as a tree

**v1 lives in corRest**, beside the HTTP server and client it mirrors, which already own request
dispatch and forwarding I/O. The communication-protocol plugin axis (a `.so`, `--protocols`) can
be cut out of it once a second protocol shows what that seam has to be - designing it from one
example would guess.

### 5.3 Multiplexing

Any number of requests in flight on one connection, answered in the order they finish, matched by
correlation id. The rule that holds it together: the tables follow the stream (§ 4.13) - frames are
**encoded in the order they are sent and decoded in the order they arrive**. Both ends run it on the
event loops' coroutines (`doc/coroutines.md`).

**Server.** A connection is armed once, for good. The loop reads every frame that comes, decodes each
request as it arrives, and runs it - inline when it cannot wait, as a coroutine when it can. Each
response is encoded as it is queued and written at once if the socket takes it; what does not fit is
flushed when the socket is writable again. A request in flight holds a reference to its connection,
so a connection that dies under running requests is freed by whichever lets go last.

**Client.** One connection a peer, per thread, shared by every request of the thread:

- the **write turn** - one frame at a time is encoded and written; a coroutine that finds the turn
  taken parks until it is its turn (corBase `corCoLoopPark/Wake`)
- the **read turn** - the first request that waits and finds no reader reads the connection; it
  decodes every frame as it comes into the memory of the call it belongs to, wakes that call's
  coroutine, and hands the turn on when its own response is in
- a response nobody waits for any more (its call timed out) is decoded all the same - the tables need
  it - and dropped

`corRestCorStart` / `corRestCorWait` send a request now and collect its response later; a distributed
operation fanned out to several cor:// sources starts every request before it waits for any, so they
are all in flight together. `corRestCorSend` is the two in one.

With `mongoc` a request that waits cannot be a coroutine (its driver blocks): its connection moves to
a thread of its own and is served one request at a time. `cor_multiplex_out_of_order` (a fast request
answered before a slow one sent earlier, on one connection) and `cor_fanout_concurrent` (two sources
of one second each, answered in one) are the functests.

## 6. What is built, and what it measures

Built: the codec (corTree) with its NGSI-LD callbacks (corNgsild); cor:// in corRest - a listener
(`--corPort`) and a client - with multiplexing (§ 5.3); forwarding over it; the corDB log and snapshot,
whose record bodies are cor trees ([corDB's persistence](https://github.com/SEAMWARE/corDB/blob/main/doc/persistence.md)).
Not yet: packed numeric arrays and timestamps as integers (§ 4.9, § 4.10). Functests: `cor_forwarding_chain`, `cor_api_direct`, the two of § 5.3;
the whole suite runs with every request over cor:// as well (`COR_TRANSPORT=cor`, `doc/testing.md`).

**Three brokers chained** (`test/perf/corChain.sh`): `corDB` brokers A -> B -> C, A and B each
registered with the next, a GET on A - every request crosses both hops twice. Each broker on two
cores, the built-in server, the release build with PGO (as the Docker image), deep idle states, two
runs each (2026-10-02):

| Entity | Mode | 1 conn req/s | p50 | 16 conns req/s | p50 | p99 |
|---|---|---|---|---|---|---|
| 4 attributes | http | 8,003-8,067 | 128 us | 42,992-44,418 | 342-349 us | 0.85-2.23 ms |
| 4 attributes | cor (hops) | 12,829-12,998 | 69 us | 79,991-80,764 | 202-203 us | 333-346 us |
| 4 attributes | **cor-all** | **11,405-11,407** | **91 us** | **91,490-91,945** | **171-172 us** | **235-237 us** |
| 20 attributes | http | 4,651-5,694 | 156-217 us | 24,415-25,127 | 631-654 us | 1.26-1.30 ms |
| 20 attributes | cor (hops) | 6,578-8,653 | 112-157 us | 39,703-40,871 | 373-383 us | 784-810 us |
| 20 attributes | **cor-all** | **6,880-6,980** | **145-147 us** | **50,988-53,642** | **292-312 us** | **398-402 us** |

- **http** - every hop HTTP and JSON; the client is wrk
- **cor (hops)** - the two forwarding hops cor://, the client still wrk over HTTP
- **cor-all** - nothing but cor://: the client is `corRequest` in its load mode

**One broker, no forwarding** - what the transport itself costs, a GET on corDB (release build, no
PGO, 2026-10-01):

| | 1 conn req/s | p99 | 16 conns req/s | p99 |
|---|---|---|---|---|
| HTTP (wrk) | ~41,000 | ~33 us | ~142,000 | 4-9 ms |
| cor:// (corRequest) | 43,400 | 31 us | 145,300 | 209 us |

Sizes on the wire: the generic codec gives 84 % of minimised JSON, with the NGSI-LD callbacks 71 %,
over 660 JSON documents of the ETSI suite and coraine's tests - payloads with compact names and
inline `@context` text, so the least favourable case; broker-to-broker traffic, with its expanded
names, is still to be measured.

## 7. Testing

- through the broker, as everything else: the three-broker chain of § 6, over `http://` and over
  `cor://`, answering identically; later, a corDB snapshot written, the broker restarted, the same
  entities answered - byte for byte, observedAt included
- `corJson -bin`: JSON → cor → JSON, which must be identical, on every expect body the functests
  have - a large corpus for free
- a malformed-input pass: a truncated buffer, a length that lies, an unknown tag. The decoder
  must reject each of them without reading past the end

## 8. Decisions

All taken - 2026-10-01:

1. **Byte order** - little-endian (§ 3).
2. **First consumer** - forwarded requests, on the three-broker chain (§ 6); the snapshot after.
3. **Testing** - through the broker, plus `corJson -bin` (§ 7); no library-level suite.
4. **The string table** - names in v1 (§ 4.5).
5. **The magic** - `C0 4F 52 01`: 0xC0 ("almost C"), `O`, `R` + version (§ 5.1).
6. **The tables' lifetime on cor://** - per connection, sent once and reused (§ 4.13).
