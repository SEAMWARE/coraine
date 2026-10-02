# The MQTT bridge

`mqtt.so`, from [corMqttBridge](https://github.com/SEAMWARE/corMqttBridge), loaded with
`--bridges mqtt`. It does two separate things.

## 1. Notifications to `mqtt://` and `mqtts://`

A Subscription whose `notification.endpoint.uri` is an MQTT URI is delivered over MQTT, as
TS 104 243 (*NGSI-LD MQTT Notification Binding*) defines it. Everything is in the Subscription:

| Where | What |
|---|---|
| `endpoint.uri` | `mqtt[s]://[<user>[:<pass>]@]<host>[:<port>]/<topic>` - ports default to 1883 / 8883 |
| `endpoint.notifierInfo` | `MQTT-QoS` (`0`, `1`, `2`, default `0`), `MQTT-Version` (`mqtt3.1.1`, `mqtt5.0`, default `mqtt5.0`) |
| `endpoint.receiverInfo` | copied into the message's `metadata` |

The message is `{ "metadata": { "Content-Type": ..., "Link": ..., ... }, "body": <the Notification> }`.
With `--insecureNotif`, an `mqtts://` endpoint's self-signed certificate is accepted.

⚠ Without `--bridges mqtt` the broker has nothing that speaks MQTT, and a Subscription with such an
endpoint is refused (400) when it is created or updated.

## 2. Channels - a topic ↔ an entity attribute

As for every bridge ([Bridges and Channels](bridge-channels.md)): a message on a topic becomes the
attribute's value, and a write to the attribute is published on the topic. The Bridge is one
connection, to the server its configuration names; each Channel is one topic. In the
`--bridgeConfig` file:

```json
{ "mqtt": { "server": { "uri": "mqtt://mosquitto:1883", "MQTT-Version": "mqtt5.0", "clientId": "coraine-plant" },
            "ngsild": { "topics": {
              "plant/tank1/level":    { "entityId": "urn:ngsi-ld:Tank:T1", "entityType": "Tank", "attribute": "level",
                                        "channelInfo": [ { "key": "MQTT-QoS", "value": "1" } ] },
              "plant/tank1/setpoint": { "entityId": "urn:ngsi-ld:Tank:T1", "entityType": "Tank", "attribute": "setpoint" } } } } }
```

- **The server** is a Subscription's MQTT URI without the topic: `uri` carries user, password,
  host and port; `MQTT-Version` is the connection's protocol version (default `mqtt5.0`);
  `clientId` is optional. The connection is opened in the background - a server that is not up
  yet does not stop the broker - and libmosquitto reconnects by itself, subscribing every inbound
  topic again.
- **A Channel's topic is exact.** No `+` or `#` wildcard: an arriving message finds its Channel by
  its topic. A wildcard Channel is refused and shows as `dormant` in `GET /ngsi-ld/v1/channels`,
  with the reason.
- **`channelInfo`** uses the keys a Subscription's `notifierInfo` does: `MQTT-QoS` (`0`, `1`, `2`).
  An unknown key refuses the Channel.
- **Payloads.** A message that parses as JSON is the value as it is (`42.5`, `{"x": 1}`); anything
  else becomes a JSON string (`running` → `"running"`). A write publishes the value's JSON.
- **No echo.** The bridge's own publication does not come back as a sample: MQTT 5's *no local*
  subscription option, and on MQTT 3.1.1 (which has none) the last payload published on the
  topic, dropped once when it returns. The same value published afterwards by a device is a sample.

Without a `server`, the Bridge delivers notifications only, and a configured Channel is refused.

## Tests

`subscription_*mqtt*` (notifications), `subscription_mqtt_endpoint_needs_bridge`, `bridge_mqtt`
and `bridge_mqtt_v311_echo` (Channels). They run a private mosquitto; `corTestClient` plays the device,
subscribing to every topic and publishing with `POST /mqtt/publish`.
