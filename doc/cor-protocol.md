# The cor format and the cor:// protocol

> **Design notes - working draft (2026-10-01), not implemented.** For discussion before any code.
> It supersedes the earlier TLV drafts, whose type codes carried NGSI-LD meaning. Here the format
> carries *trees*, and the meaning comes from the core-term ids NGSI-LD already has.

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
anywhere. (Proposed - § 8.)

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

### 4.5 A string table per message - v1 for names, later for values

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

### 4.11 Compression of a whole frame - later, off by default

A flag bit in the frame header for zstd. Pointless on a LAN after § 4.1-4.10; possibly worth it over
a WAN link to an edge node. It costs zero-copy for that frame.

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

### 4.12 An estimate - to be measured, not quoted

One entity with one attribute, as the broker holds it (expanded names, system timestamps), is about
230 bytes of JSON. With § 4.1-4.10 it is roughly 60-70 bytes. A 100-entity query result gains
further from § 4.5, because after the first entity every repeated name and type is one or two
bytes. **The first implementation measures this on the functests' real entities**, and this
section is then rewritten with those numbers.

## 5. Layer 2 - cor:// framing

Every message: a fixed 16-byte header, then one encoded tree (§ 3).

| Field | Bytes | |
|---|---|---|
| magic | 4 | `COR` + format version - resynchronisation after a framing error |
| message type | 1 | HELLO, HELLO_ACK, REQUEST, RESPONSE, PUSH, CLOSE, ERROR, PING, PONG |
| flags | 1 | bit 0: compressed (§ 4.11); bit 1: a fragment, more follows |
| reserved | 2 | zero |
| correlation id | 4 | a REQUEST's, copied into its RESPONSE; 0 for PUSH |
| length | 4 | the body's |

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

## 6. Order of work

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

## 7. Testing

- through the broker, as everything else: the three-broker chain of § 6, over `http://` and over
  `cor://`, answering identically; later, a corDB snapshot written, the broker restarted, the same
  entities answered - byte for byte, observedAt included
- `corJson -bin`: JSON → cor → JSON, which must be identical, on every expect body the functests
  have - a large corpus for free
- a malformed-input pass: a truncated buffer, a length that lies, an unknown tag. The decoder
  must reject each of them without reading past the end

## 8. Open questions

1. **Byte order** - little-endian (proposed, § 3) or network order?
2. ~~First consumer~~ **Decided:** forwarded requests, on the three-broker chain (§ 6); the
   snapshot comes after.
3. **Testing** - through the broker plus `corJson -bin` (§ 7), rather than a library-level suite?
4. **The string table** - for names only in v1 (proposed), or for values too?
5. **Name of the magic** - `COR` + version, or something that does not read as an English word in
   a hex dump?
6. ~~The tables' lifetime on cor://~~ **Decided:** per connection, sent once and reused across
   requests - the reason connections persist (§ 4.13).
