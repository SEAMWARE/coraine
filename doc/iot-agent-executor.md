# IoT Agents as Service Executors

> **Design - not built.** For review. Background: [FIWARE IoT Agents](iot-agents.md),
> [Speaking to devices directly](device-protocols.md). The iotagent-json facts are from its source at
> commit `736b443` (2026-10-14, 3.14.0-next), with `iotagent-node-lib` folded in.

An IoT Agent does three jobs. One of them - commands - is exactly what Service Execution is: a client asks
for something to be done on an entity, an executor does it, and the execution carries its status and its
result. This note is about that job: **an IoT Agent as a Service Executor**, the IoT Agent for JSON first.

| The agent's job | In coraine | Here |
|---|---|---|
| **Commands** - switch on, open a valve | a Service on the device's entity; running it is a Service Execution; the agent is its executor | **this note** |
| **Lazy attributes** - read when asked | a synchronous Service, or a registration the broker forwards a read to | open |
| **Measures** - the device pushes readings | data flowing in, as a Channel's samples | open |

## 1. What the agent does today

Writing a command attribute on the device's entity reaches the agent: the agent registers itself for it
(an NGSI-LD Context Source Registration, `mode: exclusive`), and the broker forwards the
`PATCH /ngsi-ld/v1/entities/{id}/attrs/{cmd}`. The agent keeps the command's state in two more attributes
on the entity, `<cmd>_status` (`commandStatus`) and `<cmd>_info` (`commandResult`).

| | How a command reaches the device | How the answer comes back |
|---|---|---|
| **MQTT** | published on `/<apikey>/<deviceId>/cmd`: `{"<cmd>": <value>}`; QoS and retain from the command's `mqtt` settings, else `IOTA_MQTT_QOS` / `IOTA_MQTT_RETAIN` (default 0, false) | the device publishes `{"<cmd>": <result>}` on `/<apikey>/<deviceId>/cmdexe` |
| **HTTP, push** - the device or its group has an `endpoint` | `POST <endpoint>`, the same body, `fiware-service` / `fiware-servicepath` headers | the response: 200 or 201, its body the result |
| **HTTP, pull** - no `endpoint` (`polling` is then true) | stored; the device asks with `getCmd=1` on any southbound request, and gets `{"<cmd>": <value>}` | `POST /iot/json/commands`: `{"<cmd>": <result>}` |

The status values: `PENDING` (sent, or stored for a pull), `DELIVERED` (a pull handed over), `OK`,
`ERROR`, `EXPIRED` (a pull never fetched within `IOTA_POLLING_EXPIRATION`). Source:
`lib/commandHandler.js`, `lib/bindings/MQTTBinding.js` and `HTTPBinding.js` (`executeCommand`,
`returnCommands`), `iotagent-node-lib/lib/services/commands/commandService.js`.

## 2. What coraine already has

**Service Execution** ([GR CIM-055](roadmap.md#1-service-execution)): Service Registrations
(the entities they apply to, `serviceInformation` - `serviceName`, `mode`, `inputSchema` / `outputSchema` -
`executionTimeout`, the executor's `endpoint`), executions (`POST /ngsi-ld/v1/entities/{id}/services/{name}`, `GET /ngsi-ld/v1/services`),
their status (`executing`, `completed`, `failed`, `cancelled`), progress, output and error.

**An executor inside the broker.** A Service Registration whose endpoint has a scheme a loaded bridge
claims (`BridgeDriver.serviceSchemes`, bridge ABI 11) is executed by that bridge plugin:

| Call | Direction | What |
|---|---|---|
| `serviceExecute(url, executionId, inputJson)` | broker → plugin | start one execution; non-blocking |
| `serviceUpdateIn(bridge, executionId, status, progress, output, error)` | plugin → broker | `executing`, then `completed` or `failed`; progress any time |
| `serviceCancel(url, executionId)` | broker → plugin | cancel a running one, or say it cannot be (`BRIDGE_UNSUPPORTED` → 409) |

Only the test bridge (`loopback`) implements it today.

## 3. The design: an `iota-json` executor plugin

A bridge plugin that claims the scheme **`iota-json`**. It needs no new contract - the three calls above.

### 3.1 A command is a Service Registration

One per device command:

```json
{
  "id": "urn:ngsi-ld:ServiceRegistration:lamp1-switch",
  "type": "ServiceRegistration",
  "entities": [ { "id": "urn:ngsi-ld:Lamp:lamp1", "type": "Lamp" } ],
  "serviceInformation": { "serviceName": "switch", "mode": "asynchronous" },
  "executionTimeout": "PT5M",
  "endpoint": "iota-json+mqtt://lamp-key/lamp1/switch"
}
```

**The endpoint says everything the plugin needs to deliver the command** - the transport, the apikey,
the device id, the command - so the plugin keeps no device registry for commands:

| Endpoint | Delivery |
|---|---|
| `iota-json+mqtt://<apikey>/<deviceId>/<cmd>[?qos=1&retain=true]` | MQTT publish on `/<apikey>/<deviceId>/cmd` |
| `iota-json+http://<host>[:<port>]/<path>?cmd=<cmd>` (`https` too) | HTTP push: `POST` to that URL |
| `iota-json+poll://<apikey>/<deviceId>/<cmd>` | HTTP pull: stored until the device asks |

Two ways to create them, both in one deployment:

- **Natively** - a client creates the Service Registration (`POST /ngsi-ld/v1/serviceRegistrations`), as
  for any service.
- **Drop-in** - `POST /iot/devices` with `commands: [...]`, as for iotagent-json: the plugin creates one
  Service Registration per command, and removes them with the device.

### 3.2 A command is a Service Execution

```
POST /ngsi-ld/v1/entities/urn:ngsi-ld:Lamp:lamp1/services/switch
{ "value": "on" }
```

The device receives what iotagent-json sends it - `{"switch": "on"}`: the command's name, and the input's
`value` (the whole input, without `id` and `type`, when it has no `value`).

| iotagent-json | The execution |
|---|---|
| `PENDING` | `executing` |
| `DELIVERED` (pull) | `executing`, `executionProgress: { "delivered": true }` |
| `OK` + `<cmd>_info` | `completed`, `executionOutput` = the device's result |
| `ERROR` | `failed`, `executionError` = a ProblemDetails saying why (the publish failed, the HTTP status) |
| `EXPIRED` | `failed`, "Execution Timeout" - the registration's `executionTimeout`, which the broker already enforces on every execution |
| - | `cancelled`: a pull not yet delivered is withdrawn; a delivered command cannot be (409) |

**Per transport:**

- **MQTT** - `serviceExecute` publishes and reports `executing`. The plugin subscribes to
  `/+/+/cmdexe` (with the `/json` prefix variants iotagent-json accepts); an answer `{"switch": <result>}`
  completes the oldest executing `switch` of that device. A device that never answers: the execution
  fails at its `executionTimeout`.
- **HTTP push** - the `POST` is the delivery and its response the answer: 200 / 201 completes the
  execution with the body as output; anything else fails it with the status.
- **HTTP pull** - stored; the device's `getCmd=1` hands every stored command over (`delivered`); its
  `POST .../commands` completes them. Expiry is the registration's `executionTimeout` - what
  `IOTA_POLLING_EXPIRATION` was, per service instead of per agent, and nothing the plugin does.

### 3.3 What goes away

- The `<cmd>_status` and `<cmd>_info` attributes - the execution has its status and its output, and
  `GET /ngsi-ld/v1/services?entityId=...` lists them.
- The exclusive Context Source Registration and the forwarded `PATCH` - a command is not an attribute
  write.
- The command attributes on the entity - `GET /entities/{id}?includeServices=true` shows the device's
  services (`ServiceDescription`).

## 4. The two shapes

| Shape | How the executor runs |
|---|---|
| **One coraine** | the plugin, loaded with `--bridges iota-json` |
| **cor-agent + a central broker** | the same plugin in the edge build; the central broker's Service Registration names the edge's HTTP endpoint (a plain HTTP executor), and the edge executes it |

The second needs Service Execution on the central broker - coraine has it; other brokers do not yet.

## 5. Open decisions

1. **The input** - decided: what iotagent-json sends, `{"<cmd>": input.value}` (§ 3).
2. **The endpoint carries the delivery** (no registry) - decided (2026-10-14): so for the native path; a
   device registry, if one is needed (measures map a device's keys to attributes), comes with the
   provisioning PR.
3. **Drop-in provisioning** - decided (2026-10-14): after the native path, in a PR of its own. A device
   provisioned with commands becomes one Service Registration per command, its endpoint as in § 3.
4. **The southbound HTTP listener** (port 7896) - pull commands need it, and so do HTTP measures. A
   plugin with its own listener, or a route on the broker's port?
5. **Measures and lazy attributes** - their own notes. Decided (2026-10-14): commands are Service
   Executions, lazy attributes stay a Context Source Registration - a read of data that lives in the
   device, with `attrs`, `q` and the rest of a query. iotagent-json has no lazy attribute that reaches
   a device: it sets no query handler, and iotagent-node-lib's default answers each lazy attribute with
   an empty value of its type (`contextServer-NGSI-LD.js` `defaultQueryHandlerNgsiLD`, iotagent-json
   736b443); its update handler for them is empty.

## 6. The first PR, if the above holds

- The plugin in its own repo (as the other bridges): `serviceSchemes = "iota-json+mqtt,iota-json+http,iota-json+poll"`.
- MQTT and HTTP push first; pull with the southbound listener (decision 4).
- Tests in coraine's suite: a private mosquitto, `corTestClient` as the device (it subscribes and
  publishes, as for the MQTT bridge's tests), and as the HTTP endpoint.
