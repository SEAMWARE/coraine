# Server-Sent Events - a subscription made on its own event stream

A client that wants its notifications without a WebSocket - and without a server of its own for the
broker to post to - creates its subscription with one `POST` and keeps the response open: the `201` is
a `text/event-stream` that does not end, and the subscription's notifications are its events - a
change-driven subscription's as the entities change, a periodic one's once per interval (§ 4.1). The
broker never connects to the client, and nothing but plain HTTP is needed. The client's other requests
are ordinary HTTP requests, as always.

```console
curl -N -H 'Content-Type: application/json' -d '{
  "type": "Subscription", "entities": [ { "type": "Vehicle" } ],
  "notification": { "endpoint": { "uri": "urn:ngsi-ld:self" } } }' \
  http://localhost:1026/ngsi-ld/v1/subscriptions
```

```text
: stream

event: notification
data: {"id":"urn:ngsi-ld:Notification:...","type":"Notification","subscriptionId":"urn:ngsi-ld:Subscription:...","data":[...]}

```

Server-Sent Events is the WHATWG HTML standard's `text/event-stream` (HTML § 9.2): a response that does
not end, made of events - lines of `event:` and `data:`, a blank line after each - and comments (a line
starting with `:`).

In the build by default; `-DCOR_FEATURE_SSE=OFF` leaves it out (and so does `SUBSCRIPTIONS=OFF`) -
[building](building-details.md#sse).

## 1. Next to the WebSocket

| | WebSocket (`ws.so`, [WebSocket](websocket.md)) | Server-Sent Events |
|---|---|---|
| carries | the whole API, both ways | one subscription's notifications, broker to client |
| the connection | `GET /ngsi-ld/v1/ws`, `Upgrade: websocket` | the `POST` that creates the subscription; its `201` is the stream |
| subscriptions per connection | any number, made over it | one - the one the `POST` made |
| the endpoint | `urn:ngsi-ld:self`, or the connection's id `urn:ngsi-ld:WebSocket:<run>-<n>` (both stored as the id) | `urn:ngsi-ld:self` |
| the subscription lives | as long as the connection (those notifying it) | as long as the stream |
| a client that reconnects | creates its subscriptions again; gets nothing it missed | the same |
| a killed broker's subscriptions | deleted at the next start | deleted at the next start |
| through proxies, HTTP/2 | needs `Upgrade` passed through; not over HTTP/2 | plain HTTP; a proxy must not buffer it (§ 5) |
| a browser | `WebSocket` | `fetch()` with a streamed body - `EventSource` is GET-only |
| binary | possible (`ngsi-ld.cor`, not yet) | no - text only |
| loaded | `--transports ws` (a plugin) | always, in a build with it |

Both deliver notifications to a client the broker cannot connect to; the client's other requests go
over HTTP with Server-Sent Events, over the connection with a WebSocket.

**Where Server-Sent Events is the better choice**

- **Simpler on the client.** An HTTP response read as it comes - `curl -N`, `fetch()`, any HTTP client:
  no WebSocket library, no framing, no `metadata`/`body` envelope - an event's `data` is the
  Notification.
- **One request.** The `POST` that creates the subscription is the stream: no connection to open first,
  no connection id to put in the endpoint, no `requestId` to match a response to its request.
- **Plain HTTP on the way.** No `Upgrade` for a proxy or load balancer to pass through; HTTP/2 between
  the client and a proxy in front.
- **A gateway sees the request.** The create is an ordinary `POST` with its own `Authorization` and
  `NGSILD-Tenant`, so a PEP in front of the broker (APISIX) checks it as it checks any request. Over a
  WebSocket the gateway sees the upgrade only; each message's `Authorization` is in its `metadata`,
  checked by the broker ([Authorization](authorization.md)).
- **No plugin.** In the build by default, served on the HTTP port; the WebSocket is `ws.so`, loaded with
  `--transports ws`.

**Where the WebSocket is the better choice**

- **The whole API on one connection**, both ways: requests, their responses and the notifications.
  With Server-Sent Events every other request is an HTTP request of its own.
- **Any number of subscriptions per connection.** A stream carries one; a browser over HTTP/1.1 opens
  about 6 connections per host (§ 5).
- **A subscription can move onto the connection, and off it** (`PATCH` of the endpoint). One on a stream
  is on it for its whole life.
- **Nothing to tell a proxy.** Once upgraded, frames are not buffered; a stream needs a proxy that does
  not buffer the response (`X-Accel-Buffering: no` for nginx, § 5).
- **Binary** later (`ngsi-ld.cor`); Server-Sent Events is text only.

**The same in both:** a client that reconnects creates its subscriptions again and is sent nothing it
missed; a subscription ends with its connection; a killed broker's are deleted at its next start. c³
uses the WebSocket.

It is **not a transport** - a transport carries the API ([WebSocket](websocket.md#how-it-is-built)); this
carries one subscription's notifications. It is not a notification binding either: there is no address
the broker sends to, the notification is written on a response that is already open.

## 2. `urn:ngsi-ld:self`

`urn:ngsi-ld:self` names the connection the subscription is made on. On an HTTP `POST` that is its event
stream - this document. Over a WebSocket it is that WebSocket: an alias of the connection's id, which the
broker stores in its place - the subscription then shows the id, and is the connection's as one made with
the id is ([WebSocket](websocket.md#subscriptions)). The id keeps working beside it.

## 3. Creating it

`POST /ngsi-ld/v1/subscriptions` or `POST /ngsi-ld/v1/csourceSubscriptions`, on the broker's HTTP port,
`notification.endpoint.uri` `urn:ngsi-ld:self`. The endpoint alone decides; no header is needed.

```text
HTTP/1.1 201 Created
Location: /ngsi-ld/v1/subscriptions/urn:ngsi-ld:Subscription:...
Content-Type: text/event-stream
Cache-Control: no-cache
X-Accel-Buffering: no
Transfer-Encoding: chunked
```

| | Status |
|---|---|
| an `Accept` without `text/event-stream`, `text/*` or `*/*` (no `Accept` is fine) | 406 |
| over cor:// - the request has no HTTP response to stream (over a WebSocket it names the connection, § 2) | 400 |
| `--sseStreams` streams open already, or the broker stopping | 503 |

What else a subscription is refused for, it is refused for here too, before the stream: the `201` is
the stream, an error is an ordinary response.

`Accept: text/event-stream` on a subscription with another endpoint is an ordinary create: its `201` is
JSON, so an `Accept` that names only `text/event-stream` is a 406, as any `Accept` without JSON is.

- `notification.endpoint.accept`: `application/ld+json` (the default with this endpoint) puts the
  `@context` in each notification's body - an event has no `Link` header to carry it; the core context
  when the subscription names none. `application/json` leaves it out.
- The subscription shows its endpoint as `urn:ngsi-ld:self` (`GET /subscriptions/{id}`).
- `timesSent`, `lastNotification` and `lastSuccess` move when an event is written on the stream (a
  periodic subscription of entities: § 4.1).
- A `PATCH` does not change the endpoint to or from `urn:ngsi-ld:self` (400): a subscription is on its
  stream for its whole life. Everything else is patched as on any subscription: a periodic one stays
  periodic and a change-driven one change-driven - adding or removing `timeInterval` is a 400 - and the
  interval may change.

## 4. The stream

- **The first bytes**, at once: the comment `: stream`. A notification made before the stream is open -
  the subscription is in the cache before its `201` is out - waits for it (up to 1000 of them).
- **An event** per notification: `event: notification`, one `data:` line - the Notification (or
  ContextSourceNotification) as JSON on one line - and a blank line. No `id:`: a client that reconnects
  creates a new subscription, there is nothing to resume.
- **A comment** (`: keep-alive`) after 15 s with nothing sent, so that a proxy does not close an idle
  stream.
- **The client leaves** - closes the connection, or is killed: the broker sees it within a second, and
  deletes the subscription.
- **The broker ends it** when the subscription is deleted (`DELETE`), when the client falls 16 MiB
  behind (the connection is cut, the subscription deleted), and when the broker stops (every stream
  ended, every subscription on one deleted).
- **A killed broker** deletes nothing. At its next start every subscription and context source
  subscription on `urn:ngsi-ld:self` is deleted, in every tenant - no stream outlives the broker that
  served it. Not with `--high-availability`: there, a stream may be another instance's, open.

Both HTTP servers serve it: libmicrohttpd (a response from a content reader, its connection suspended
while there is nothing to send) and the built-in one (corHttp's response stream).

A browser page reads it with `fetch()` - `EventSource` sends a `GET`:

```js
const r = await fetch('/ngsi-ld/v1/subscriptions', { method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ type: 'Subscription', entities: [ { type: 'Vehicle' } ],
                         notification: { endpoint: { uri: 'urn:ngsi-ld:self' } } }) });
const reader = r.body.pipeThrough(new TextDecoderStream()).getReader();
let text = '';
for (;;) {
  const { value, done } = await reader.read();
  if (done) break;                                 // the stream ended: the subscription is gone
  text += value;
  let end;
  while ((end = text.indexOf('\n\n')) >= 0) {
    const block = text.slice(0, end); text = text.slice(end + 2);
    const data = block.split('\n').find(l => l.startsWith('data: '));
    if (data) console.log(JSON.parse(data.slice(6)));
  }
}
```

With `--auth` the request carries its token as every request does (`Authorization`); the stream is the
response to the create, so it is authorized as the create is.

### 4.1 Periodic subscriptions

A subscription with `timeInterval` (seconds) is made on its stream the same way, for entities and
for context sources:

```console
curl -N -H 'Content-Type: application/json' -d '{
  "type": "Subscription", "entities": [ { "type": "Vehicle" } ], "timeInterval": 10,
  "notification": { "endpoint": { "uri": "urn:ngsi-ld:self" } } }' \
  http://localhost:1026/ngsi-ld/v1/subscriptions
```

- **The first event** within a second of the `201`, then one per interval - the matching entities as
  they are then, as over HTTP. Two events are never closer than the interval.
- **Entities**: an interval in which no entity matches sends no event (the keep-alive comments still
  come). **Context sources**: an event every interval, its `data` empty when no registration matches
  (§ 12.4.7).
- **The counters** of an entity subscription move when the event is handed to the stream, as they move
  when it is sent over HTTP; a context source subscription's, when it is written.
- Its lifetime is a stream's (above): the client leaves, it is deleted.

## 5. Deployment

- **A buffering proxy holds events back.** nginx buffers unless told not to; `X-Accel-Buffering: no`
  tells it, per response. APISIX is nginx.
- **HTTP/1.1 in a browser: about 6 connections per host**, and each open stream is one. A page reading
  many subscriptions wants HTTP/2 in front, or one subscription for them all.
- A stream is a connection for as long as it is open: `--sseStreams` caps them (default 1000; one more
  is a 503).

Metrics (`/admin/metrics`, with the admin API and `COR_FEATURE_METRICS`): `ngsild_sse_streams` (the
streams there - open, or their `201` on its way), `ngsild_sse_events_total` (the notifications written
as events), `ngsild_sse_streams_refused_total` (the `503`s: `--sseStreams` streams there, or the broker
stopping).

## 6. How it is built

- **corRest** (`corRestStream.h`, `COR_WITH_STREAM`): a response whose body is written over time, from
  any thread - `corRestStreamResponse`, `corRestStreamWrite`, `corRestStreamEnd`. A write of 0 bytes
  writes nothing and says whether the client is still there.
- **corNgsild** (`ldNotifyStream.h`, `COR_WITH_EVENT_STREAM`): the endpoint `urn:ngsi-ld:self`, and the
  hook a notification on it is handed to, rendered - by each of its senders: entity subscriptions,
  context source subscriptions (both change-driven and periodic) and periodic entity subscriptions
  (`ldPernotLoop`); the counters move when the application says an event was written
  (`ldNotifyStreamSent`), the periodic entity sender's at the hand-over (`ldNotifyStreamHand`).
- **coraine** (`src/lib/sse`, `COR_FEATURE_SSE`): the streams, one entry per subscription under one
  mutex; a thread that, once a second, looks whether each client is still there, writes the keep-alives,
  updates the counters and deletes the subscriptions whose streams ended (a `DELETE` run in the broker).

## 7. NGSI-LD

Not in the specification: its bindings are HTTP and MQTT (and coraine's WebSocket). It solves what
MQTT solves - a broker that cannot reach the client - with no second server and no WebSocket, and is
worth proposing to ETSI: the endpoint value `urn:ngsi-ld:self`, the stream as the response to the create.

## 8. Not yet

- Service Executions' notifications (`POST /services`) on a stream
