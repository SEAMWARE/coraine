# A Modbus bridge — design proposal

> **v1 built** (2026-10-01): Modbus TCP, the address in the endpoint and how to read it in `channelInfo` (§ 2.1), change-only reporting
> with a deadband (§ 2.3), queued writes (§ 2.4), `modbusStatus` on a device that stops answering
> (§ 2.5) - `src/plugins/bridge/modbus`, tested against a Modbus device in the functests
> (`test/funcTests/tools/ftModbus.py`, `bridge_modbus.test`). **Not yet:** contiguous registers read
> together (§ 2.2), RTU, read-on-demand through a registration (§ 3.5), a way back for a write's
> outcome (§ 3.4 - logged only). The questions in § 5 are still open; v1 takes the proposed answer
> to each, so changing one is a change to v1, not a design restart.
>
> Originally written as the proposal below, to decide the shape before any code.
> The point is less Modbus itself than what it does to the Bridge/Channel seam:
> it is the opposite of DDS in almost every respect, so wherever the seam only
> works because DDS is DDS, a Modbus plugin will find it.

## 1. Modbus in one page

A 1979 industrial protocol, still in most PLCs, energy meters, inverters, pumps
and HVAC controllers.

- **A master polls, a slave answers.** Nothing is ever pushed: a value changes
  inside the device and nobody knows until someone asks.
- **No names, no types.** A device exposes four numbered tables:
  | Table | Width | Access | Function codes |
  |---|---|---|---|
  | coils | 1 bit | read/write | 1 read, 5 write one, 15 write many |
  | discrete inputs | 1 bit | read only | 2 |
  | holding registers | 16 bit | read/write | 3 read, 6 write one, 16 write many |
  | input registers | 16 bit | read only | 4 |

  That "registers 30001-30002 are the grid frequency, a 32-bit float, word order
  CDAB, in 0.01 Hz" exists only in the device's manual.
- **Transport:** Modbus TCP (port 502; a 7-byte MBAP header in front of the PDU)
  or RTU over serial lines (RS-485, with a CRC). One TCP gateway often fronts
  many serial devices, told apart by the **unit id**.
- **Errors** are exception responses (illegal function, illegal address,
  device busy), or silence - a timeout.

## 2. Onto the seam

| Bridge/Channel | Modbus |
|---|---|
| **Bridge** - a transport instance, few, fixed at startup | one Modbus server: `host:port` (TCP), or one serial line |
| **Channel** - one foreign endpoint <-> one attribute | one value in the device: a table, an address, a length, and how to read the bits |
| `sampleIn(endpoint, json, time)` | a poll result - the decoded value, at the time it was read |
| `publish(endpoint, json)` | a write to a coil or holding register(s) |

### 2.1 The endpoint is the address; channelInfo says how to read it

DDS endpoints are names (`rt/pose`), and the type comes with the data. A Modbus device says nothing
about what its registers mean, so a Channel needs more than an address - but the endpoint is the
Channel's **identity** (an arriving sample finds its Channel by it), so it carries *where* the value is
and nothing else:

```
[unit/<n>/]coil|discrete|holding|input/<address>

holding/40001        unit/3/holding/40001        coil/12        input/30010
```

The address is 0-based, or the 1-based `1xxxx`/`3xxxx`/`4xxxx` form of the device manuals, recognised
by its range. *How* to read the value, and how to carry it, is the Channel's **`channelInfo`** - an
array of key-value pairs, as NGSI-LD's `receiverInfo` / `notifierInfo` / `contextSourceInfo` are for an
endpoint of theirs. Changing a scale factor is then not a different Channel:

| key | Meaning | Default |
|---|---|---|
| `type` | `bool`, `int16`, `uint16`, `int32`, `uint32`, `float32`, `float64` | `bool` for bits, `uint16` for registers |
| `order` | word/byte order for multi-register values: `ABCD`, `CDAB`, `BADC`, `DCBA` | `ABCD` |
| `scale`, `offset` | value = raw x scale + offset (inverted on write) | 1, 0 |
| `poll` | poll period, ms (inbound) | the Bridge's |
| `deadband` | only report a change larger than this | 0 = any change |

Both key and value are strings. An unknown key, or a value that does not fit (a `float32` coil), is
not a default: the plugin refuses the Channel, which then shows as `dormant` in `GET /channels` with the
reason. The broker never interprets a key - it hands `channelInfo` to the plugin as it is
(`channelAddInfo`, bridge ABI 9), and `GET /channels` shows it.

### 2.2 Inbound: polling is the plugin's job

The plugin owns a timer per poll period and reads what is due. Contiguous
registers of one unit due at the same time are read in ONE request (a meter's
30 values in one function-3 read, not 30), which is the difference between a
usable and an unusable bridge on a slow serial line.

`publishTime` is the time the read completed - Modbus has no timestamps.

### 2.3 Change-only, or the broker drowns

A DDS publisher sends when something happens. A poll returns the same 21.5
every second for hours, and each `sampleIn` is an attribute write, a
subscription match, possibly notifications and a TRoE row. So the plugin
reports a value only when it differs from the last one reported (beyond
`deadband`), plus - optionally - a heartbeat every N polls so a consumer can
tell "unchanged" from "dead".

Deciding this in the plugin rather than the broker is deliberate: only the
plugin knows the value is a poll of an unchanged register and not a new
event that happens to repeat.

### 2.4 Outbound: a write queue

`publish()` runs on a broker thread inside the request that changed the
attribute and must not block (BridgeDriver.h). A Modbus write is a network
round trip - 5 ms on a LAN, 100+ ms through a serial gateway - so `publish()`
encodes the value (inverse scale, type, order) and queues it; the plugin's I/O
thread sends it. `json` for a coil is `true`/`false`; for a register a number;
anything else is BRIDGE_BAD_INPUT at publish time.

### 2.5 The device is gone

A timeout or an exception on a poll: the attribute keeps its last value (it is
the last KNOWN value) and gets `sampleMetaIn` meta once, when the state changes:

```
"modbusStatus": { "type": "Property", "value": "timeout" }     # or "ok", "illegalAddress", ...
```

## 3. What it tests in the seam

This is the part worth the work. Expected findings, each a decision to make
rather than a fix to slip in:

1. **Channel parameters - done: `channelInfo`.** A Channel had an endpoint, a kind and a direction.
   Modbus needs typing and scaling; the first version put them in the endpoint string, which made a
   scale factor part of the Channel's identity, and left every future transport (OPC-UA sampling, MQTT
   QoS and codec) to invent its own URL syntax. Now the endpoint is the address and `channelInfo` - key-value
   pairs, as `receiverInfo` - is the rest, handed to the plugin by `channelAddInfo` (bridge ABI 9).
2. **Who owns time.** DDS carries source timestamps; Modbus none; the seam's
   `publishTime = 0` already means "broker, use your clock". OK as is.
3. **Change detection.** Push transports do not need it, poll transports all do
   (Modbus, OPC-UA reads, SNMP, BACnet). Plugin-side as above - but it is worth
   deciding whether the broker should offer it as a service to every plugin.
4. **Write outcomes.** `publish()` returns before the write happens. A device
   that answers "illegal address" has no way back into the broker today except
   the log. DDS has the same gap and hides it (a DDS write rarely fails).
   Candidate: an upcall `publishResultIn(endpoint, status)` that sets meta on
   the attribute, like 2.5.
5. **Delegation (lazy reads).** device-protocols.md: a value read on demand
   belongs in a registration whose endpoint names the bridge, not in a Channel.
   A GET forwarded to `modbus://...` - one read, one reply - is exactly the
   Service kind (request, one reply). Not in v1, but the first real user of
   "a registration pointing at a bridge".
6. **Many Channels, one request.** DDS maps one topic to one Channel; Modbus
   wants the plugin to batch many Channels into one read. The seam allows it
   (the plugin schedules), but a Channel set that should be read together
   (one meter) has no name. Possibly: the entity is the natural group.

## 4. Implementation

Configuration - the Bridge's member of the `--bridgeConfig` file; `ngsild.topics` ties each endpoint to
an attribute, exactly as for every other bridge:

```json
{ "modbus": { "server": { "host": "10.0.0.5", "port": 502, "unit": 1, "pollMs": 1000, "timeoutMs": 500 },
              "ngsild": { "topics": {
                "holding/0": { "entityId": "urn:Meter:1", "entityType": "Meter", "attribute": "power",
                               "channelInfo": [ { "key": "type",  "value": "float32" },
                                                { "key": "order", "value": "CDAB" },
                                                { "key": "scale", "value": "0.1" } ] },
                "coil/3": { "entityId": "urn:Meter:1", "entityType": "Meter", "attribute": "relay" } } } } }
```

Run with `--bridges modbus --bridgeConfig <file>`.


- **Own Modbus TCP client**, not libmodbus: the protocol is a 7-byte header and
  eight function codes - a few hundred lines - and it keeps the plugin free of a
  dependency. RTU (serial, CRC) as a second step; that is where libmodbus would
  earn its place, if at all.
- **Plugin threads:** one I/O thread per Bridge (one TCP connection; Modbus TCP
  allows pipelining with transaction ids, but many devices answer one at a
  time - v1 sends one request at a time).
- **Test device:** a small Modbus TCP server in the test tools (`ftModbus`) with
  its registers set and read over HTTP, like ftClient. That makes functests of
  polling, change-only reporting, writes, exceptions and timeouts deterministic.

## 5. Questions

1. ~~Mapping in the endpoint, or ...~~ **Decided: `channelInfo`** (§ 2.1). Still open: a named mapping
   (one `channelInfo` for a fleet of identical devices), referenced by the Channels that use it.
2. Change-only in the plugin (proposed), or a broker-side service for all poll
   transports?
3. Write outcomes: `publishResultIn` upcall now, or log-only in v1?
4. RTU in v1, or TCP only?
