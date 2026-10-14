# Service Execution

"Do this" in the NGSI-LD API: switch a lamp on, start a robot's mission, run a computation on an
entity. A **service** is offered on entities; running it is a **Service Execution**, which an
**executor** carries out and which has its own status, progress, output and error. coraine follows
ETSI GR CIM-055 (*Service Execution*), which is still being settled - see [What is provisional](#what-is-provisional).

Compiled in by default; `COR_FEATURE_SERVICE_EXECUTION=OFF` removes it and its 17 routes answer 501
([Building](building-details.md#service_execution)). The feature needs a store plugin with a document
store (corDB, mongoc); otherwise its routes answer 422.

## The pieces

| | What it is | Where |
|---|---|---|
| **Service Registration** | "entities like these offer service X; this endpoint executes it" | `/ngsi-ld/v1/serviceRegistrations` |
| **ServiceDescription** | the same, as an attribute of one entity | the entity itself |
| **Service Execution** | one run of a service on one entity | `/ngsi-ld/v1/services`, `/ngsi-ld/v1/entities/{id}/services/{name}` |
| **Combined / Grouped Service Execution** | several runs, in sequence or in parallel, or one per selected entity | `POST /ngsi-ld/v1/services` |
| **Combined Service Template** | a reusable combination | `/ngsi-ld/v1/combinedServiceTemplate` |
| **Executor** | whoever does the work: an HTTP endpoint, or a loaded bridge plugin | the registration's `endpoint` |

## Service Registrations

```json
{
  "id": "urn:ngsi-ld:ServiceRegistration:SR1",
  "type": "ServiceRegistration",
  "endpoint": "http://lamps.example.org/setLight",
  "entities": [ { "idPattern": "^urn:MyLamp", "type": "Lamp" } ],
  "serviceInformation": {
    "serviceName": "setLight",
    "title": "setLight",
    "description": "Set brightness of light, 0 means off",
    "mode": "synchronous",
    "inputSchema": { "type": "object", "properties": { "brightness": { "type": "integer", "minimum": 0, "maximum": 255 } }, "required": [ "brightness" ] },
    "outputSchema": { "type": "string" }
  }
}
```

| Member | |
|---|---|
| `type` | `ServiceRegistration` - required |
| `endpoint` | the executor - required. `http`/`https`, or a scheme a loaded bridge executes (below); any other scheme is refused (400) |
| `entities` | required, non-empty: `{ "type", "id" \| "idPattern" }` each - `type` required, not both `id` and `idPattern` |
| `serviceInformation` | required: `serviceName` (required, expanded like an attribute name), `title`, `description`, `mode` (`synchronous` - the default - or `asynchronous`), `inputSchema`, `outputSchema` (a JSON Schema object, or `true` / `false`) |
| `q`, `scopeQ`, `geoQ` | narrow the entities further, on the entity as stored |
| `executionTimeout` | ISO 8601 duration; default 30 s for a synchronous service, 1 h for an asynchronous one |
| `notification` | `{ "endpoint": { "uri": ... } }` - where the executions' changes are notified |
| `id` | optional; generated `urn:ngsi-ld:ServiceRegistration:...` |

**A service name is unique within an entity.** A registration whose `serviceName` is already offered by
another registration whose `entities` could select the same entity is refused, 409, naming the other.
Two selectors are separate only when different types, different ids, or an id the other's `idPattern`
does not match prove it; `q`, `scopeQ` and `geoQ` do not separate them. An entity two registrations
still both select (one with two types, say) gets 409 when the service is invoked.

| Route | |
|---|---|
| `POST /ngsi-ld/v1/serviceRegistrations` | 201, `Location`; 409 for an existing id or a name conflict |
| `GET /ngsi-ld/v1/serviceRegistrations` | all of them; with `?type` (and `?id`): the services an entity of that type (and id) is offered - with `q`, `scopeQ` and `geoQ` evaluated when the entity exists |
| `GET /ngsi-ld/v1/serviceRegistrations/{id}` | 200, 404 |
| `PATCH /ngsi-ld/v1/serviceRegistrations/{id}` | each member replaces the stored one, the whole checked again; 204 |
| `DELETE /ngsi-ld/v1/serviceRegistrations/{id}` | 204, 404 |

## An entity's services

`GET /ngsi-ld/v1/entities/{id}?includeServices=true` (and `GET /ngsi-ld/v1/entities`) shows each service
the entity is offered as an attribute named by the service:

```json
"setLight": { "type": "ServiceDescription", "title": "setLight", "description": "Set brightness of light, 0 means off",
              "mode": "synchronous", "serviceDescriptionInEntity": false }
```

`&serviceDetails=true` adds `inputSchema` and `outputSchema`. The executor's endpoint is never shown.
Without `includeServices=true` nothing is added.

An entity can also carry a service itself: an attribute of type `ServiceDescription` with an `endpoint`
(required), `mode`, `title`, `description` and the schemas. It is hidden unless `includeServices=true`
(then with `serviceDescriptionInEntity: true`), is used before any registration, and has the default
timeout and no notification.

## Running a service

```
POST /ngsi-ld/v1/entities/urn:MyLamp1/services/setLight
{ "brightness": 128 }
```

The body is the service's **input**, a JSON object - checked against `inputSchema` (400, and no
execution, when it does not fit). The schema keywords checked: `type`, `properties`, `required`,
`additionalProperties`, `items`, `enum`, `minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum`,
`minLength`, `maxLength`, `minItems`, `maxItems`; any other is ignored.

| Mode | Answer |
|---|---|
| **synchronous** | the broker waits for the executor: **200**, the body the execution's output |
| **asynchronous** | **202**, at once - `Location` and `Service-Execution: <execution id>`; follow it at `GET /ngsi-ld/v1/services/{id}` or by notification |

`POST /ngsi-ld/v1/services` with `{ "type": "ServiceExecution", "entityId", "serviceName", "executionInput" }`
does the same and always answers **201** with `Location` and `Service-Execution` - also for a synchronous
service.

**An input member can read an attribute** when the execution starts:

```json
{ "level": { "type": "AttributeSelector", "attributeSelector": "brightness", "entityId": "urn:MyLamp2" } }
```

`attributeSelector` is an attribute (`brightness`), a sub-attribute (`brightness.unit`) or a member of a
JSON value (`config[mode][night]`); without `entityId` it is the execution's own entity. The member is
replaced by the attribute's value (or `object`); one that names nothing fails the execution (400). Only
top-level input members are read this way, and the schema checks the input as written.

### The execution

```json
{
  "id": "urn:ngsi-ld:ServiceExecution:6acfb4f3:000a",
  "type": "ServiceExecution",
  "entityId": "urn:MyLamp1",
  "entityType": "Lamp",
  "serviceName": "setLight",
  "executionInput": { "brightness": 128 },
  "executionStatus": "completed",
  "executionOutput": "done",
  "executionStartedAt": "2026-10-14T20:00:00.120Z",
  "executionEndedAt": "2026-10-14T20:00:00.135Z"
}
```

`executionStatus` is `pending`, then `executing` (an asynchronous service, from when it is handed over),
then one of `completed`, `failed` or `cancelled`.
`executionProgress` is whatever the executor reports while it runs; `executionError` is a ProblemDetails.
`createdAt` / `modifiedAt` with `?options=sysAttrs`.

| Route | |
|---|---|
| `GET /ngsi-ld/v1/services` | every execution; `?entityId`, `?serviceName`, `?executionStatus` (exact match) |
| `GET /ngsi-ld/v1/services/{id}` | one; a combined or grouped one with its children inside, as `serviceExecutions` |
| `GET /ngsi-ld/v1/entities/{id}/services/{name}/{executionId}` | the same, for that entity and service |
| `DELETE /ngsi-ld/v1/services/{id}` (or under the entity) | running: cancelled (204), or 409 when the executor cannot stop it; ended: deleted (204) |

- **Timeout** - an execution that has not ended within its registration's `executionTimeout` fails:
  "Execution Timeout", 504. Checked about every 5 seconds.
- **Retention** - an ended execution is deleted 24 hours after it ended.
- **Notifications** - every change of an execution (executing, progress, its end) is notified to the
  execution's `notification.endpoint.uri` - from `POST /services`, else the registration's:
  `{ "id", "type": "Notification", "notifiedAt", "data": [ <the execution> ] }`, compacted with the
  @context of the request that started it. A non-HTTP URI goes through a transport
  ([WebSocket](websocket.md)).

## Executors

### An HTTP executor

The broker hands an execution over with

```
POST <endpoint>
Service-Execution: <execution id>
NGSILD-Tenant: <tenant>          (when there is one)
Content-Type: application/json

{ "id": "urn:MyLamp1", "type": "Lamp", "brightness": 128 }
```

- the entity's `id` and `type` (compacted), and the input members, selectors read;
- connect timeout 5 s, response timeout the execution's.

| Mode | The executor answers |
|---|---|
| synchronous | 2xx, its body the output (JSON, or text - then a JSON string) |
| asynchronous | 2xx (202) to accept; then reports with `PATCH /ngsi-ld/v1/services/{id}` |

**A report** - `PATCH /ngsi-ld/v1/services/{id}`, with the header `Service-Execution: {id}` and only
these members: `executionStatus` (`executing`, `completed`, `failed`, `cancelled`), `executionProgress`,
`executionOutput`, `executionError` (required with `failed`, a ProblemDetails). 204; 409 once the execution
has ended.

**Cancel** - `DELETE <endpoint>/<execution id>`, the same headers: 2xx cancelled; 409 "it goes on".

| The executor | The client gets |
|---|---|
| does not answer in time | 504 Executor Timeout |
| cannot be reached | 503 Executor Unreachable |
| answers 4xx | that status, with the executor's ProblemDetails when it sent one |
| answers anything else | 502 Executor Refused |

The execution is stored as failed, with the same ProblemDetails, and the error carries `Service-Execution`.

### A bridge executor

A registration whose endpoint has a scheme a loaded bridge plugin claims (`BridgeDriver.serviceSchemes`,
bridge ABI 11) is executed by that plugin, in the broker: `serviceExecute`, `serviceCancel`, and the
plugin's reports through `BridgeBroker.serviceUpdateIn` - `executing`, `completed` (an output) or `failed`
(a ProblemDetails), progress any time. The loopback bridge (`--bridges loopback`) executes
`loopback://echo`, `progress`, `fail`, `hold` and `stuck`, for tests
([Bridges and Channels](bridge-channels.md)).

| The bridge | The client gets |
|---|---|
| has no such service | 503 |
| cannot use the input | 400 |
| refuses | 502 |
| a synchronous service, failed | 502, the bridge's ProblemDetails |
| a synchronous service, no answer in time | 504 - and the broker cancels it |

## Combined and grouped executions

`POST /ngsi-ld/v1/services`, 201, `Location`, `Service-Execution`. Every child is checked before anything
is stored: one refusal, and nothing is created. At most 255 executions per request.

**Combined** - a list of executions, run by `combinationMethod`:

```json
{ "type": "CombinedServiceExecution", "combinationMethod": "sequential",
  "serviceExecutions": [
    { "type": "ServiceExecution", "entityId": "urn:R1", "serviceName": "dock" },
    { "type": "ServiceExecution", "entityId": "urn:R1", "serviceName": "charge", "executionInput": { "to": 90 } } ] }
```

- `sequential` - one after the other; a child that fails or is cancelled fails the whole, and the
  children not started are cancelled.
- `parallel` - all at once; completed when every child completed, failed otherwise.
- A child can itself be a `CombinedServiceExecution` (without its own `notification`).

**From a template** - `{ "type": "CombinedServiceExecution", "serviceTemplateId", "services": [ { "entityId",
"serviceName"?, "executionInput"? } ] }`: the template's services with these entities, position by position.
A template is `{ "type": "CombinedServiceTemplate", "templateName", "combinationMethod", "services": [
{ "serviceName", "entityType", "entityId"?, "executionInput"? } ] }` at `/ngsi-ld/v1/combinedServiceTemplate`
(POST 201, GET, PATCH 204, DELETE 204).

**Grouped** - one service, on every entity a selector picks that offers it:

```json
{ "type": "GroupedServiceExecution", "combinationMethod": "parallel", "serviceName": "switchOff",
  "entityType": "Lamp", "q": "floor==3" }
```

Selectors: `entityType`, `idPattern`, `q`, `geoQ`, `scopeQ` - at least one, all of them holding. An
entity that does not offer the service is skipped; 404 when none does.

Cancelling a running combined or grouped execution cancels its running children (409 if one cannot be);
deleting an ended one deletes its children too.

## What is provisional

GR CIM-055 is not final. Expect changes in the status names and the query and discovery parameters
([roadmap](roadmap.md)). Not built: the combination methods `conditional`, `timetriggered` and
`recurring` (501, "still to be specified"). `GET /ngsi-ld/v1/services` is this broker's own - the report
lists it as missing. Trace level 403 (`CtService`) traces executions.
