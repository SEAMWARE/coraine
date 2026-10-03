# One way to say "this went wrong" — unified error reporting for NGSI-LD

Raw material for the **ETSI TC DATA face-to-face, Athens, 20–22 October 2026**,
for the same deck as [Typed ProblemDetails](problem-details.md). That one is
about what ONE error can *say* (its members); this one is about how a response
*carries* errors — which body, which status code, one error or many, the same
rule for every endpoint. Not implemented. Clause numbers: TS 104-175 ("175",
the API), TS 104-176 ("176", the HTTP binding).

> **Five shapes for "part of this failed", and a client has to know which
> endpoint it called to know which one it got.**

## Today

- **Five shapes for one idea**: ProblemDetails (175 § 8.3.3), OperationResult
  `{success, errors[EntityError]}`, UpdateResult `{updated,
  notUpdated[NotUpdatedResult]}` — whose `reason` is a free-text String with no
  type or status — ExecutionResult (snapshots), and the NGSILD-Warning header.
- **Similar operations, different shapes**: Merge Entity answers with the entity
  shape while Append and Update Attributes use the attribute shape; Set
  Attribute Value and Replace Attribute differ; temporal operations require
  partial success and have no 207 to carry it.
- **Five ways a distributed failure comes back** — and distributed *queries*
  have none.
- **`Conflict` has no HTTP status**; 508 and 502 have no error type.
- **Coraine, measured**: two 207 bodies, extension `entityId`/`entityIds` on a
  shared-status batch error, and `registrationId` in four different places.

## The proposal

**One rule for the status code**, for every operation:

| Outcome | Status | Body |
|---|---|---|
| everything succeeded | the operation's own success code | as today |
| exactly one thing failed and nothing succeeded | that error's own status | one ProblemDetails |
| nothing succeeded, and every failure is a 404 | 404 | one ProblemDetails naming every target not found |
| anything else | **207 Multi-Status** | **ErrorReport** |

**One body for a partial result — ErrorReport**: `errors` is an array of
complete ProblemDetails, each with its own `type`, `status` and identifying
members (`entityIds`, `attributeNames`, `datasetIds`, `registrationIds`);
`success` lists the targets that went well, with the same identifying members.
EntityError and NotUpdatedResult disappear: a ProblemDetails *is* the element.
Names are objects, alias → Fully Qualified Name; identifiers are arrays of
URIs. Flat: a forwarding broker appends downstream errors to its own, extending
each one's `registrationIds`.

```json
{
  "errors": [
    {
      "type": "https://uri.etsi.org/ngsi-ld/errors/ResourceNotFound",
      "title": "Entity not found",
      "status": 404,
      "detail": "…",
      "entityIds": [ "urn:ngsi-ld:Vehicle:B" ]
    }
  ],
  "success": [
    { "entityIds": [ "urn:ngsi-ld:Vehicle:A" ] }
  ]
}
```

**Every endpoint** with a partial outcome — entity, attribute, batch, temporal
(where 207 is new) and Purge Snapshots (which gains `snapshotIds`) — has the
same three columns: success code, one ProblemDetails, or a 207 ErrorReport.

**Distributed reads** keep their data: (a) NGSILD-Warning names the
registration and covers every distributed GET and `POST /entityOperations/query`
now; (b) on request, the body wraps the data as `{ "data": [...], "errors":
[...] }` as the direction.

**Asynchronous outcomes**: subscriptions and registrations gain `lastError`
(a ProblemDetails behind `lastFailure`); snapshots' ExecutionResult carries
`errors`, not `problemDetails`.

**Compatibility**: ErrorReport replaces OperationResult and UpdateResult — a
breaking change, introduced behind the core @context version the client
advertises (`;v1.10`). Status-code-only clients are affected only where the status
rule above turns a 207 into a 4xx or the reverse; the new ProblemDetails members are
additive.

## What Coraine would change

One response builder for every partial outcome instead of two;
`registrationIds` always inside the ProblemDetails; NGSILD-Warning with the
registration id on every distributed GET; `lastError` on subscriptions and
registrations. Its functional suite pins every current shape, so the size of
the change can be counted rather than estimated.

## Open questions

Strings or objects in `success`; whether "all 404 is one 404" extends to any
shared error; header or body for distributed reads; whether `ErrorReport` or
`OperationResult` is the name.

The full reference: [error-reporting-details.md](error-reporting-details.md).
