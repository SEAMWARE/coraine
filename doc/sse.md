# Server-Sent Events - a subscription's notifications as an event stream

> **Design - not built.** For review. Background: [WebSocket](websocket.md) (the API and its
> notifications over one connection), [Authorization in the broker](authorization.md).

A client that only wants notifications - a dashboard, a browser page, a script behind NAT - opens one
plain `GET` on the subscription and reads its notifications as they happen. The broker never connects to
the client, as with a WebSocket; unlike a WebSocket, the stream is plain HTTP, and a client that loses it
reconnects and is sent what it missed.

```console
curl -N -H 'Accept: text/event-stream' http://localhost:1026/ngsi-ld/v1/subscriptions/urn:ngsi-ld:Subscription:s1/events
```

```text
id: 6f1c0a9e2b7d4e18-1
event: notification
data: {"id":"urn:ngsi-ld:Notification:...","type":"Notification","subscriptionId":"urn:ngsi-ld:Subscription:s1",...}

```

Server-Sent Events is the WHATWG HTML standard's `text/event-stream` (HTML § 9.2): a response that does
not end, made of events - lines of `id:`, `event:` and `data:`, a blank line after each. Browsers read it
with `EventSource`, which reconnects by itself and sends the last id it saw in `Last-Event-ID`.

## 1. What it is next to the WebSocket

| | WebSocket (`ws.so`) | Server-Sent Events |
|---|---|---|
| carries | the whole API, both ways | one subscription's notifications, broker to client |
| subscription made over | the connection itself | anything - HTTP, cor://, a WebSocket |
| the subscription lives | as long as the connection | as long as the subscription - the stream comes and goes |
| a client that reconnects | gets nothing it missed | is sent the notifications since `Last-Event-ID` (§ 4) |
| through proxies, HTTP/2 | needs `Upgrade` passed through; not over HTTP/2 | plain HTTP; a proxy must not buffer it (§ 6) |
| binary | possible (`ngsi-ld.cor`, not yet) | no - text only |

The WebSocket stays what c³ uses. Server-Sent Events is for the clients that read and never write.

It is **not a transport**: a transport carries the API ([WebSocket](websocket.md#how-it-is-built)), this
carries notifications only - a notification binding, beside HTTP and MQTT. Its own feature flag,
`COR_FEATURE_SSE`, from the first PR; off leaves out the route, the buffers and the endpoint value.

## 2. The subscription

A subscription is delivered on an event stream when its endpoint says so:

```json
"notification": { "endpoint": { "uri": "urn:ngsi-ld:EventStream", "accept": "application/ld+json" } }
```

- `urn:ngsi-ld:EventStream` - one fixed value: the stream is found by the subscription's id, there is
  nothing else to name. Any other value is delivered as today.
- `accept`: `application/ld+json` (the default here) puts the `@context` in each notification's body - an
  event has no `Link` header to carry it; `application/json` leaves it out, for a client that knows its
  context.
- Allowed for context source subscriptions too (`/csourceSubscriptions/{id}/events`).

A subscription with this endpoint and no stream open is not failing: its notifications wait in its buffer
(§ 4), and `status` stays `active`. `timesSent` and `lastNotification` count what was written to a stream.

## 3. The stream

`GET /ngsi-ld/v1/subscriptions/{id}/events`, `Accept: text/event-stream`, on the broker's HTTP port.

| | Status |
|---|---|
| the subscription does not exist, in the request's tenant | 404 |
| its endpoint is not `urn:ngsi-ld:EventStream` | 400 |
| a stream is already open on it | 409 - one reader per subscription |
| `Accept` without `text/event-stream` | 406 |

The response: `200`, `Content-Type: text/event-stream`, `Cache-Control: no-cache`,
`X-Accel-Buffering: no`; then the buffered notifications the client has not seen (§ 4), then each new one
as it is made.

- **An event** per notification: `id:` (§ 4), `event: notification`, one `data:` line - the
  Notification, as JSON on one line (the broker's serializer writes no newlines).
- **A comment** (`: keep-alive`) every 15 s with nothing sent, so that a proxy does not close an idle
  stream.
- **`event: gap`** when notifications were lost - the buffer overflowed, or `Last-Event-ID` is of another
  run of the broker (§ 4). Its `data:` says how many, when the broker knows. The client then reads the
  current state with a query, as after any start.
- **The broker closes** the stream when the subscription is deleted, its tenant is deleted, or the broker
  stops; a client that does not read for 5 s is cut, as on a WebSocket. The subscription stays.

Both HTTP servers serve it. A stream is a response that never ends, which neither does today: corRest
gets a **stream hook** of its own - the route answers the headers, then the broker takes the socket and
writes events on it, on one thread per stream (the thread-per-connection invariant). It is the same
hand-over as the upgrade (`corRestSetUpgradeHook`), but not the upgrade: no `Upgrade`, no `101`, its own
hook and its own counter. libmicrohttpd: its upgrade API cannot take a `200`, so a response from a
callback with the connection suspended between events, or the socket taken; to be decided on the code.

## 4. Replay - `Last-Event-ID`

Each subscription with this endpoint keeps its latest notifications in a ring in RAM:
`--sseBuffer` notifications (default 100; 0: none, a reconnect gets only new ones). The event id is
`<run>-<n>`: the run is the broker's random per-start part (as in a WebSocket connection's id), `n` counts
the subscription's notifications from 1.

| `Last-Event-ID` | The broker sends |
|---|---|
| none | the ring from its oldest - what was made while no one read |
| this run, `n` still in the ring | everything after `n` |
| this run, `n` older than the ring | `event: gap` (how many were lost), then the ring |
| another run | `event: gap` (unknown), then the ring |

The ring is not persisted: after a restart a client resumes with a gap, never with a silent loss. Replay
from the store's log - notifications made again from the changes after the last one sent - is the
durable form, and later (§ 8).

## 5. Who may read it

The stream is a read of the subscription: `authorize` decides it as for `GET /subscriptions/{id}`, and
the reader must be the subscription's **owner** (the identity it was made with, as `notify` uses since
ABI 5). Without `--auth` anyone who has the id may read it - as anyone may `GET` it today.

The problem is the browser: `EventSource` sends **no headers of its own** - no `Authorization`, no
`NGSILD-Tenant`. Three ways, and the choice is open:

| | Header token | Query token | Cookie |
|---|---|---|---|
| how | `fetch()` with a streamed body, `Authorization` set; reconnect written by the client (~30 lines) | `?access_token=` (RFC 6750 § 2.3), with `EventSource` | the provider's session cookie |
| cost | no `EventSource` | the token in access logs, proxies, browser history | a session the broker does not have |

Proposal: the header always; the query token **off** by default, on with `--sseQueryToken`, and only on
this route. The tenant, for `EventSource`: `?tenant=` on this route only, with the header winning.

## 6. Deployment

- **A buffering proxy holds events back.** nginx buffers unless told not to (`X-Accel-Buffering: no` does
  it per response); APISIX is nginx - to be checked with the `ngsi-ld-pep` plugin in front.
- **HTTP/1.1 in a browser: about 6 connections per host**, and each open stream is one. A page reading
  many subscriptions wants HTTP/2 in front, or one subscription for them all.
- A stream is a socket and a thread for as long as it is open: counted in `/metrics`, and capped with
  `--sseStreams` (default 1000; one more is 503).
- The rings are RAM that grows with the subscriptions on a stream endpoint: counted in the memory budget
  (`--memoryLimit`), and over it a new notification overwrites the oldest one in its ring, as a full ring
  does.

## 7. NGSI-LD

Not in the specification: its bindings are HTTP and MQTT (and coraine's WebSocket). It solves for a
reader what MQTT solves - a broker that cannot reach the client - without a second server, and is worth
proposing to ETSI once it runs: the endpoint value, the route, the event and its id.

## 8. Not in the first version

- one stream for several subscriptions (`GET /ngsi-ld/v1/events?subscriptions=...`)
- replay from the store's log, across restarts
- more than one reader per subscription
- Service Executions' notifications (`POST /services`) on a stream
