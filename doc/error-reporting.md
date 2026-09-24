# One way to say "this went wrong" — unified error reporting for NGSI-LD

Raw material for the **Athens TC DATA face-to-face, 20–22 October 2026**, for the
same deck as *Typed ProblemDetails* (`doc/problem-details.md`). The two are
related and not the same:

- **ProblemDetails** is about what ONE error can *say*: its members.
- **This** is about how an NGSI-LD response *carries* errors: which body, which
  status code, one error or many, and the same rule for every endpoint.

This one depends on the other in exactly one place: the uniform shape below only
works if a ProblemDetails can say which Entity, which Attribute and which
Registration it is about. That is the member set proposed in `problem-details.md`
§ 9.

Draft 2026-09-24. Clause numbers are TS 104-175 ("175", the API) and TS 104-176
("176", the HTTP binding).

The one-line provocation for the slide:

> **Five shapes for "part of this failed", and a client has to know which
> endpoint it called to know which one it got.**

---

## 1. What we have today

### 1.1 Five shapes for one idea

| Shape | Defined | Element | What identifies the failed thing | The error itself |
|---|---|---|---|---|
| **ProblemDetails** (whole request) | 175 § 8.3.3 | — | nothing structured (`instance`, optional) | `type` + `title` + `detail` |
| **OperationResult** `{success, errors}`, called *BatchOperationResult* in 176 | 175 § 5.2.6.8.1 | **EntityError** `{entityId, error, registrationId?}`, called *BatchEntityError* | `entityId`, `registrationId` | a full ProblemDetails in `error` |
| **UpdateResult** `{updated, notUpdated}` | 175 § 5.2.6.8.5 | **NotUpdatedResult** `{attributeName, reason, registrationId?}` | `attributeName`, `registrationId` | **a free-text String** `reason` — no type, no status |
| **ExecutionResult** `{resultStatus, problemDetails?}` (snapshots) | 175 § 5.2.6.8.3 | — | its *position* in a list | a ProblemDetails, named `problemDetails` |
| **NGSILD-Warning** header (distributed GET) | 176 § 6.3.5 | — | nothing (no registration) | a warning code (110/111/199/299) and a sentence |

Plus two that are *status*, not response: **NotificationParams** (`status`,
`lastFailure`, `timesFailed` — 175 § 5.2.6.7.3) and **CSourceRegistration**
(`status`, `lastFailure`, `timesFailed` — 175 § 5.2.6.5.3), neither of which
says *what* failed.

The same idea has three member names — `error`, `problemDetails`, `reason` — and
three container pairs — `success`/`errors`, `updated`/`notUpdated`,
`…QueriesDetails`.

### 1.2 Similar operations, different shapes

The shape does not follow from what the operation does:

- **Merge Entity** (176 § 7.3.3.2) answers a partial failure with the
  *entity* shape (OperationResult) — but its output in 175 § 10.2.9.5 is "List of
  Attributes … actually merged", and its siblings **Append** (§ 7.4.3.1) and
  **Update Attributes** (§ 7.4.3.2) use the *attribute* shape (UpdateResult).
- **Update Attributes** (176 § 7.4.3.2) contradicts itself: the row says
  UpdateResult, its remark says a distributed result "is returned in a
  *BatchOperationResult* structure".
- **Replace Attribute** (§ 7.5.3.3) uses UpdateResult; **Set Attribute Value**
  (§ 7.6.3.2), one level down on the same Attribute, uses OperationResult.
- **Delete Attribute** (§ 7.5.3.4) uses UpdateResult, whose `updated` is defined
  as "appended or updated".
- **Purge Snapshots** (176 § 13.2.3.2) reuses OperationResult for Snapshot IDs,
  whose element's member is called `entityId`.
- **Temporal operations** (175 § 11.2.2.4 – 11.2.7.4) require "partial success"
  handling — and 176 § 8.x gives them no 207 and no body to carry it.
- Several 175 "Output data" sections say **None** (Replace Entity, Delete Entity,
  Replace Attribute, Delete Attribute, Purge…) while their behaviour sections
  require partial success and 176 defines a 207 body for them.

### 1.3 Five ways a *distributed* failure comes back

1. `registrationId` inside EntityError / NotUpdatedResult — optional.
2. The **NGSILD-Warning** header — GET/HEAD on `/entities` and `/entities/{id}`
   only, naming no registration, and absent from every endpoint table.
3. A bare **508 / 504 / 404 / 502** for a single exclusive or redirect source
   (176 § 6.3.5) — 508 and 502 have no NGSI-LD error type.
4. The registration's own `status` / `lastFailure` / `timesFailed`, out of band.
5. `Conflict` entries added to `errors` for a registration that does not support
   the operation.

Distributed **queries** — `POST /entityOperations/query`, temporal queries,
`/types`, `/attributes` — have no defined way to report a failed registration at
all.

### 1.4 Error types without a status, statuses without a type

- **Conflict** is defined (175 § 8.3.2) and used everywhere a registration does
  not support an operation — and is **absent from the HTTP mapping** (176
  § 6.3.2). (Also in `problem-details.md` as a standalone CR.)
- **508** and **502** (176 § 6.3.5) have no error type; **504** is otherwise
  mapped only to `LdContextNotAvailable`.

### 1.5 What an implementation ends up doing — Coraine, measured

Coraine implements the spec, so it inherits all of the above, and its functional
tests pin them. From its expectations:

- **two** 207 bodies — `{success, errors[{entityId, error, registrationId?}]}` and
  `{updated, notUpdated[{attributeName, reason, registrationId?, statusCode?}]}`
  — the second with a loose `statusCode` next to a String, because the element has
  nowhere to put one;
- a batch whose every element failed the same way answers that single status with
  a ProblemDetails carrying `entityId` / `entityIds` — an extension, because the
  standard one cannot say which;
- `registrationId` (singular, where § 4.A of the ProblemDetails notes plan plural
  arrays) in **four** places: inside a ProblemDetails (single forwarded
  retrieve), beside it (EntityError), beside a String (NotUpdatedResult), and
  nowhere at all on a distributed read, where the warning header names the
  source's `host:port` instead;
- `NGSILD-Warning` carrying both a failed registration and an unrelated notice
  ("DateTime values were rounded to nanosecond resolution").

Every one of those is a decision the spec left to the implementation, and every
implementation decides it differently.

---

## 2. The proposal

### 2.1 One rule for the status code

For **every** operation, local or distributed, entity or attribute or batch:

| Outcome | Status | Body |
|---|---|---|
| everything succeeded | the operation's own success code (200/201/204) | as today |
| **exactly one** thing failed and nothing succeeded | **that error's own status** (400, 404, 409, …) | **one ProblemDetails** |
| **nothing succeeded, and every failure is a 404** | **404** | **one ProblemDetails**, naming every target that was not found (`entityIds`, `attributeNames`) |
| anything else — failures and successes, or failures that differ | **207 Multi-Status** | **ErrorReport** (§ 2.2) |

**All 404 is one 404.** A request whose every target turned out not to exist has
one answer, and it is "not found": a 207 listing *N* identical ResourceNotFound
elements says the same thing *N* times and makes the client dig for it. So it is
a single 404 whose ProblemDetails names them all — which is exactly what the
plural identifying members (`problem-details.md` § 9.2) are for. The same holds
for one entity whose distributed parts were not found at any source.

"Anything else" is the catch-all: a request where some things succeeded, or where
the failures are of different kinds, has no single honest status, and 207 is
what says so. (Today the all-404 case is open to interpretation — 176's batch
tables name 207 for a partial result, while Coraine answers a shared status with a
ProblemDetails — and this settles it.)

### 2.2 One body for a partial result: ErrorReport

```json
{
  "errors": [
    {
      "type": "https://uri.etsi.org/ngsi-ld/errors/ResourceNotFound",
      "title": "Entity not found",
      "status": 404,
      "detail": "…",
      "entityIds": [ "urn:ngsi-ld:Vehicle:B" ]
    },
    {
      "type": "https://uri.etsi.org/ngsi-ld/errors/Conflict",
      "title": "Operation not supported by the registration",
      "status": 409,
      "detail": "…",
      "entityIds": [ "urn:ngsi-ld:Vehicle:C" ],
      "attributeNames": [ "speed" ],
      "registrationIds": [ "urn:ngsi-ld:ContextSourceRegistration:7" ]
    }
  ],
  "success": [
    { "entityIds": [ "urn:ngsi-ld:Vehicle:A" ] },
    { "entityIds": [ "urn:ngsi-ld:Vehicle:C" ], "attributeNames": [ "brand" ] }
  ]
}
```

- **`errors`** — an array of **ProblemDetails**, each one complete: its own
  `type` and `status`, and the members saying what it is about (`entityIds`,
  `attributeNames`, `datasetIds`, `registrationIds` — `problem-details.md` § 4.A
  and § 9; plural, because one error can be about several things, as a batch with
  a duplicated ID is).
  The wrapper element (EntityError) and the String element (NotUpdatedResult)
  both disappear: a ProblemDetails *is* the element.
- **`success`** — what went well, as **targets**: objects with the same
  identifying members an error has, and nothing else. An entity operation lists
  `{entityIds}`; an attribute operation lists `{entityIds, attributeNames}` (or
  just `{attributeNames}` where the URL names the entity). The same member names
  as in `errors`, so a client matches a success and an error on the same keys.
- **Flat.** Siblings and a distributed chain are the same array; a broker that
  forwarded appends the downstream errors to its own, extending each one's
  `registrationIds` with the registration it forwarded through
  (`problem-details.md` § 5).

**Open: strings or objects in `success`.** Strings are smaller and match today's
OperationResult.success, but only if every endpoint's unit is one kind of
identifier, and attribute operations need the entity *and* the attribute. The
object form is the one shape that serves both. The room should choose, knowing
that strings bring back a second shape.

### 2.3 Every endpoint

| Endpoint | Operation (175) | Success | One failure | Partial → 207 ErrorReport, identified by |
|---|---|---|---|---|
| `POST /entities` | Create Entity § 10.2.2 | 201 | ProblemDetails | `entityIds` (+ `attributeNames` when a distributed part refused some) |
| `PATCH /entities/{id}` | Merge Entity § 10.2.9 | 204 | ProblemDetails | `entityIds` + `attributeNames` |
| `PUT /entities/{id}` | Replace Entity § 10.2.10 | 204 | ProblemDetails | `entityIds` + `attributeNames` |
| `DELETE /entities/{id}` | Delete Entity § 10.2.8 | 204 | ProblemDetails | `entityIds` (+ `registrationIds`) |
| `DELETE /entities` | Purge Entities § 10.2.12 | 204 | ProblemDetails | `entityIds` |
| `POST /entities/{id}/attrs` | Append Attributes § 10.2.4 | 204 | ProblemDetails | `attributeNames` |
| `PATCH /entities/{id}/attrs` | Update Attributes § 10.2.3 | 204 | ProblemDetails | `attributeNames` |
| `PATCH /entities/{id}/attrs/{attrId}` | Partial Attribute Update § 10.2.5 | 204 | ProblemDetails | `attributeNames` (+ `datasetIds`) |
| `PUT /entities/{id}/attrs/{attrId}` | Replace Attribute § 10.2.11 | 204 | ProblemDetails | `attributeNames` (+ `datasetIds`) |
| `PUT …/attrs/{attrId}/value` | Set Attribute Value § 10.2.6 | 204 | ProblemDetails | `attributeNames` (+ `datasetIds`) |
| `DELETE /entities/{id}/attrs/{attrId}` | Delete Attribute § 10.2.7 | 204 | ProblemDetails | `attributeNames` (+ `datasetIds`) |
| `POST /entityOperations/create` | § 10.3.2 | 201 | ProblemDetails | `entityIds` |
| `POST /entityOperations/upsert` | § 10.3.3 | 201 / 204 | ProblemDetails | `entityIds` |
| `POST /entityOperations/update` | § 10.3.4 | 204 | ProblemDetails | `entityIds` (+ `attributeNames`) |
| `POST /entityOperations/merge` | § 10.3.5 | 204 | ProblemDetails | `entityIds` (+ `attributeNames`) |
| `POST /entityOperations/delete` | § 10.3.6 | 204 | ProblemDetails | `entityIds` |
| `POST /temporal/entities`, `…/attrs`, `DELETE …` | § 11.2.x | 201 / 204 | ProblemDetails | `entityIds` / `attributeNames` — **207 is new here**; 175 already requires partial success |
| `DELETE /snapshots` | Purge Snapshots § 16.7 | 204 | ProblemDetails | `snapshotIds` — the member Purge has been missing |
| everything without a partial outcome (subscriptions, registrations, `/types`, `/attributes`, `/jsonldContexts`, single GETs) | — | as today | ProblemDetails | — |

Every row has the same three columns and the same body in the last one. That is
the whole of the proposal; the rest is consequences.

### 2.4 Distributed reads

A GET returns *data*, and a partial failure on a query or a retrieve cannot turn
the response into an ErrorReport without losing the data. Two options for the room:

- **(a) Keep the header, make it say which registration** — NGSILD-Warning's value
  gains the `registrationId` (instead of, or beside, the host), keeps the warning
  codes, and is extended beyond `/entities` to every distributed GET and to
  `POST /entityOperations/query`. Compatible; still not a ProblemDetails.
- **(b) Offer the errors in the body, on request** — with `Prefer:
  ngsi-ld-errors` (or an `options` value) the response body wraps the data:
  `{ "data": [...], "errors": [ProblemDetails…] }`. Same `errors`, same
  elements as every other endpoint; opt-in, so nothing breaks.

Recommendation: **(a) now, (b) as the direction.** (a) is a clarification; (b)
is the only way a query's partial failure ever carries a ProblemDetails.

### 2.5 Asynchronous outcomes

Subscriptions, registrations and snapshots report failure long after the request
that caused it. Their status members stay — they answer "how is it doing" — but
each gains **`lastError`: ProblemDetails**, the error behind `lastFailure`:
today a subscription can say *that* its last notification failed, and never *why*.
Snapshots' ExecutionResult becomes `{ resultStatus, errors: [ProblemDetails] }`
— `errors`, not `problemDetails`, the same name as everywhere else.

### 2.6 Compatibility

- ErrorReport **replaces** OperationResult and UpdateResult. That is a breaking
  change for any client that reads `notUpdated` or `errors[].error`, and should
  be introduced the way the spec introduces any response-shape change: behind the
  core @context version (`;v1.10`) the client advertises, the old shapes staying
  for older contexts.
- Clients that only look at the **status code** are unaffected, except where the
  "exactly one failure" rule changes a 207 into a 4xx, or the reverse — § 2.1's
  table is the full list.
- The single-error ProblemDetails is **additive**: the new members are optional
  to read.

---

## 3. Housekeeping the room should fix regardless

Found while cataloguing, independent of whether the proposal is taken:

- *OperationResult* (175) vs *BatchOperationResult* (176, defined nowhere);
  *EntityError* vs *BatchEntityError*; *ExecutionResult* vs
  *ExecutionResultDetails*, cited with two different clause numbers.
- Four spellings of the snapshot details members: `snapshotTemporalQueriesDetails`,
  `temporalSnapshotQueriesDetails`, `snapshotQueryDetails`,
  `snapshotTemporalQueryDetails`.
- `snapshotStatus` "preparing" (§ 5.2.6.5.4) vs "preparation" (§ 16.2.4, 16.3.4).
- 175 § 12.4.7 sets "the subscription status to failed", but Subscription.status
  allows only active / paused / expired — it means `notification.status`.
- `timesFailed` is "0 or greater" in CSourceRegistration and "Greater than 0" in
  NotificationParams.
- 176 § 6.3.3 refers to itself for the status code; it means § 6.3.2.
- Create Entity's 207 row repeats the 201 row's Location-header remark.
- Batch upsert's output (175 § 10.3.3.5) says "none" where 176 § 7.8.3.1 also
  returns 201 with the IDs.

---

## 4. What Coraine would change

- One response builder for every partial outcome, replacing the two it has
  (batch `errors`/`success` and attribute `updated`/`notUpdated`) — the per-endpoint
  status logic collapses into § 2.1's three lines.
- `registrationIds` inside the ProblemDetails, always (§ 1.5 lists the four places
  it is today).
- NGSILD-Warning with the registration id, and on every distributed GET.
- `lastError` on subscriptions and registrations: it already knows the error when
  it records `lastFailure`, and throws it away.

It is also the implementation that can bring measured numbers to the room: its
functional suite pins every one of the shapes in § 1, so the size of the change —
and which tests a unified shape would simplify — can be counted rather than
estimated.

## 5. Open questions

1. Strings or objects in `success` (§ 2.2).
2. **Does "all 404 is one 404" extend to any shared error?** Every element a 409
   Conflict, or a 400 for the same reason: one ProblemDetails naming them all, or a
   207? 404 is the clear case — "nothing you named exists" is one fact. A shared
   400 or 409 may still differ in its `detail` per element, which a single
   ProblemDetails would have to drop.
3. Header or body for distributed reads (§ 2.4), and whether (b) is a `Prefer` or
   an `options` value.
4. Is `ErrorReport` the right name — or does the room prefer to keep
   `OperationResult` for the unified shape, since that is the one clause 5
   actually defines?
