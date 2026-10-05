# WebSocket - subscriptions and notifications over one connection

> **Design, for review - nothing of it is built yet.** Version 1 is what a web page needs: connect,
> create a subscription over the connection, receive its notifications on it.

A web page opens a WebSocket to the broker, sends NGSI-LD requests over it and receives the responses
and, for the subscriptions it created, the notifications - on the same connection, with nothing to
poll and no endpoint of its own for the broker to reach.

## 1. Where it lives: a transport, not a bridge

A Bridge carries **attribute values** between a foreign endpoint and an entity, and never answers a
request ([Bridges and Channels](bridge-channels.md)). A WebSocket here carries the broker's **API** -
requests in, responses and notifications out - as HTTP and cor:// do. So it is a **transport**, and
transports get a plugin type of their own: `ws.so` first, cor:// to follow (`cor.so`, its server and
its client for forwarding), HTTP itself later.

| | `ws.so` | `cor.so` (later) |
|---|---|---|
| how a connection starts | an upgrade on the HTTP port | its own port (`--corPort`) |
| the broker serves requests | yes | yes |
| the broker as a client | no | yes - forwarding |
| framing | WebSocket frames, the JSON envelope | cor binary |

The seam, both ways:

- broker → plugin: `init` (its options); **take this connection** (an upgraded socket, the bytes the
  HTTP server already read behind the request included); **send** an envelope on a connection
- plugin → broker: **request** (an envelope in, the response envelope out - through the same service
  routines as HTTP); **opened** / **closed** (the broker keeps the connections subscriptions name)

The plugin knows WebSocket and the envelope; the broker knows NGSI-LD. Neither crosses the line.

## 2. Connecting

`GET /ngsi-ld/v1/ws` with `Upgrade: websocket` on the broker's HTTP (or HTTPS) port, the
`Sec-WebSocket-Protocol` `ngsi-ld.json`. Both HTTP servers hand the socket over after `101 Switching
Protocols`: libmicrohttpd with its upgrade API (`MHD_create_response_for_upgrade`), corHttp with an
upgrade hook added to it - same contract, so the plugin sees no difference. The WebSocket protocol
itself (RFC 6455: the handshake's key, framing, masking, ping/pong, close) is `ws.so`'s, not
libmicrohttpd's experimental `libmicrohttpd_ws`: the built-in server gets it too.

The broker's first message on a new connection names it:

```json
{ "metadata": { "connection": "urn:ngsi-ld:WebSocket:7" } }
```

## 3. Messages: the envelope of the MQTT binding

Every message is a JSON object with `metadata` and `body` - the envelope of ETSI TS 104 243 (the
MQTT notification binding), so a notification over WebSocket is byte for byte the one MQTT carries.

**A request** - its method and path in `metadata`, with the headers HTTP would carry, and a
`requestId` the page chooses, returned in the response:

```json
{ "metadata": { "method": "POST", "path": "/ngsi-ld/v1/subscriptions", "requestId": "1",
                "Content-Type": "application/json" },
  "body": { "type": "Subscription", "entities": [ { "type": "Vehicle" } ],
            "notification": { "endpoint": { "uri": "urn:ngsi-ld:WebSocket:7" } } } }
```

**A response** - the status and the headers HTTP would return:

```json
{ "metadata": { "status": 201, "requestId": "1", "Location": "/ngsi-ld/v1/subscriptions/urn:ngsi-ld:Subscription:..." } }
```

**A notification** - as on MQTT, `Content-Type` and `Link` in `metadata`, the Notification in `body`.

Several requests may be in flight; a response says which one it answers by its `requestId`.

## 4. A subscription notifies a connection

A subscription whose `endpoint.uri` is a connection's id is notified on that connection.
corNgsild already hands every non-HTTP notification to one transport hook (`ldNotifyTransportSend`),
which wraps it in the envelope and gives it to the plugin for its URI - `mqtt://` to `mqtt.so` today;
`urn:ngsi-ld:WebSocket:` to `ws.so`. At subscription create and update, the same hook says whether
the URI can be delivered (`ldNotifyTransportHas`): an id that names no open connection is refused.

## 5. Version 1

- connect, both HTTP servers
- `/ngsi-ld/v1/subscriptions` over the connection: create, list, retrieve, update, delete - any other
  path answered `501` naming what version 1 serves
- notifications to the connection that created the subscription
- the connection closes: the subscriptions whose endpoint is it are deleted (an id never comes back)

## 6. For review

1. **Another connection as the endpoint.** May a page create a subscription that notifies a
   connection other than its own (a second socket of the same page, another page)? Version 1: no -
   only its own connection's id is accepted. Allowing it needs a rule: the same tenant, the same
   credentials (once there are any)?
2. **On close: delete, or keep?** Deleting is simple and leaves nothing behind. Keeping would need a
   connection to come back under its old id - a resume token, later if ever.
3. **The path** - `/ngsi-ld/v1/ws`, or a path outside the NGSI-LD tree (`/ws`)?
4. **Tenant**: the `NGSILD-Tenant` of the upgrade request, for the whole connection, or per message in
   `metadata`? Version 1: per connection.

## 7. Later - roadmap, not short term

- every request over the connection, not only subscriptions
- a second subprotocol, `ngsi-ld.cor`: binary frames carrying the same envelope cor-encoded - cor's
  encoding and multiplexing through proxies and ingresses that block cor://'s own port
- cor:// moved into the transport plugin type (`cor.so`)
- a WebSocket **bridge** (device values through Channels), if a device use case comes
