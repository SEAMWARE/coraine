# WebSocket - the broker's API and its notifications over one connection

A web page opens a WebSocket to the broker, sends it any request it would send over HTTP, creates
subscriptions over it and receives their notifications on it - with nothing to poll and no endpoint of
its own for the broker to reach.

```console
coraine --transports ws
```

In the build by default; `-DCOR_FEATURE_TRANSPORTS=OFF` leaves it out - no `--transports`, no `ws.so`
([building](building-details.md)).

Served over the connection: every request the broker serves over HTTP - entities, batch operations,
subscriptions, context source registrations and subscriptions, types and attributes, `@context`
documents, temporal, `/version`, and the API plugins' paths. Notifications - of subscriptions and of
context source subscriptions - go to the connection that created the subscription.

## Connecting

`GET /ngsi-ld/v1/ws` on the broker's HTTP port, with `Upgrade: websocket` (RFC 6455, version 13). The
subprotocol `ngsi-ld.json` may be asked for (`Sec-WebSocket-Protocol`), and is then confirmed; none
asked for is the same. Both HTTP servers serve it - libmicrohttpd and the built-in one.

The broker's first message names the connection:

```json
{ "metadata": { "connection": "urn:ngsi-ld:WebSocket:7" } }
```

| Refused upgrade | Status |
|---|---|
| no `Sec-WebSocket-Key`, a version other than 13, a subprotocol other than `ngsi-ld.json` | 400 |
| NGSI-LD headers - `NGSILD-Tenant`, `Link`, `NGSILD-Snapshot` - in the upgrade request | 400 |
| a path other than `/ngsi-ld/v1/ws` | 404 |

## Messages

Text messages, each a JSON object with `metadata` and `body` - the envelope of the MQTT notification
binding (ETSI TS 104 243 clause 5), so a notification over a WebSocket is the one MQTT carries.

**A request** names its method and path in `metadata`, beside the headers HTTP would carry. A
`requestId`, the client's choice, comes back in the response; several requests may be in flight.

```json
{ "metadata": { "method": "POST", "path": "/ngsi-ld/v1/subscriptions", "requestId": "r1",
                "Content-Type": "application/json" },
  "body": { "type": "Subscription", "entities": [ { "type": "Vehicle" } ],
            "notification": { "endpoint": { "uri": "urn:ngsi-ld:WebSocket:7" } } } }
```

**A response** carries the status, the `requestId` and the headers HTTP would return, and the body -
a JSON body as it is, any other (the text of `/metrics`) as a JSON string:

```json
{ "metadata": { "status": 201, "requestId": "r1",
                "Location": "/ngsi-ld/v1/subscriptions/urn:ngsi-ld:Subscription:..." } }
```

**A notification** - `Content-Type` and `Link` in `metadata`, the NGSI-LD Notification (or
ContextSourceNotification) in `body`.

A request runs through the same service routines as over HTTP: what it can be refused for, and how,
is the same. Over a WebSocket besides:

| | Status |
|---|---|
| a message that is not a JSON object with a `metadata` object | 400 |
| a request without `method` or `path` | 400 |
| a subscription whose endpoint names another connection | 403 |

## Tenants

A request's tenant is its own `NGSILD-Tenant` in `metadata`; without one, the default tenant - as over
HTTP. A connection has no tenant, nor any other NGSI-LD header: every message carries its own, and a
message means the same on every connection. An upgrade request with `NGSILD-Tenant`, `Link` or
`NGSILD-Snapshot` is refused (400). A browser cannot set headers on an upgrade request anyway.

## Subscriptions

A subscription or context source subscription created over a connection notifies that connection: its
`notification.endpoint.uri` is the connection's id. Another connection's id is refused. When the
connection closes - the client closes it, it breaks, or the broker stops - the subscriptions it
created and did not delete are deleted, each in its tenant.

## The connection

- text frames; a fragmented message (a frame and its continuations) is one message; a ping is answered
  with a pong; a close frame with the close
- a binary frame closes the connection (1003); a frame the client did not mask (1002); a message over
  16 MiB (1009)
- a notification to a client that stopped reading gives up after 5 s

## How it is built

WebSocket is a **transport**: it carries the broker's API, as HTTP and cor:// do - not a bridge, which
carries attribute values and answers no request ([Bridges and Channels](bridge-channels.md)).
Transports are a plugin type of their own (`src/lib/plugin/TransportDriver.h`), loaded from
`<plugins>/transport/` by `--transports`; `ws.so` is the first.

- **The plugin** knows the protocol: the handshake (`Sec-WebSocket-Accept`, SHA-1 from libcrypto), the
  frames, its connections - one thread each.
- **The broker** knows the messages: it parses a request's envelope, runs it to its end on the
  connection's thread (corRest's `corRestRunJson` - the service routines of an HTTP request, JSON in and
  out), answers in an envelope, keeps the subscriptions each connection created, and delivers the
  notifications to a connection through corNgsild's transport hook - the one `mqtt://` goes through.
- **The upgrade**: corRest asks the application for an `Upgrade` request (`corRestSetUpgradeHook`),
  writes the `101`, and hands the socket over - with libmicrohttpd's upgrade API, or corHttp's
  `corHttpUpgrade` on the built-in server, the same contract.

## Not yet

- a subscription notifying another connection than its own
- a second subprotocol, `ngsi-ld.cor`: binary frames carrying the envelope cor-encoded
- cor:// as a transport plugin (`cor.so`)
