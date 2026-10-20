# Server-Sent Events - history

How the event stream came to be the response to the subscription's `POST`. What it is today is in
[Server-Sent Events](../sse.md). Newest first.

## 2026-10-20 - built: the stream is the create, the endpoint `urn:ngsi-ld:self`

The first design (below) read a subscription's notifications with a `GET` on
`/subscriptions/{id}/events`. KZ asked whether the stream had to be a `GET` - it does not: only the
browser's `EventSource` is GET-only, and `fetch()` reads a streamed body of any request. The stream
became the `201` to the `POST` that creates the subscription, and with it went:

- **the stream key** - the creator holds the connection, there is nothing to hand over;
- **the ring and `Last-Event-ID` replay** - the subscription dies with its stream ("dies with it",
  KZ), so a client that reconnects makes a new one; no `id:` on the events;
- **`?tenant=`** - the `POST` carries `NGSILD-Tenant` as any request does;
- **the owner check under `--auth`** - the stream is authorized as the create is;
- **one reader per subscription (409)** - one stream, the creator's.

The endpoint was first `urn:ngsi-ld:EventStream` together with a required `Accept: text/event-stream`
(a 400 when one came without the other). KZ chose `urn:ngsi-ld:self` - the connection the subscription
is made on, to mean the same over a WebSocket later - and the endpoint alone decides; `Accept` is
checked only as HTTP content negotiation (406).

A client that left was first noticed only by the keep-alive: libmicrohttpd does not watch a suspended
connection, and the first write after the client closed still succeeded, so the subscription stayed
for up to two keep-alive periods (30 s). corRest's write of 0 bytes now peeks at the socket, once a
second.

## 2026-10-19 - the first design: a `GET` on the subscription

> A client that only wants notifications opens one plain `GET` on the subscription and reads its
> notifications as they happen. [...] A client that loses it reconnects and is sent what it missed.

- endpoint `urn:ngsi-ld:EventStream`, the stream at `GET /ngsi-ld/v1/subscriptions/{id}/events?key=...`;
- a 128-bit stream key given once in an `Event-Stream` header of the `201`, its SHA-256 in the store, a
  `PATCH` to the endpoint again made a new one;
- a ring of `--sseBuffer` notifications (default 100) per subscription, event ids `<run>-<n>`,
  `Last-Event-ID` replay and an `event: gap` when some were lost;
- one reader per subscription (409; a second reader "newest wins" was decided, then made moot);
- with `--auth`, the reader had to be the subscription's owner, its token in `Authorization`.
