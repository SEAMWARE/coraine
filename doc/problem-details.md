# Typed ProblemDetails — material for an ice-breaker

Raw material for a **slide deck to open a discussion at the ETSI TC DATA
face-to-face, Athens, 20–22 October 2026**. Not a contribution and not a CR —
the question for the room is *what should an NGSI-LD error be able to say?* —
except for the missing `Conflict` status code (below), which is a clean
standalone CR. The companion piece, on how a response *carries* errors, is
[error-reporting.md](error-reporting.md). Neither is implemented.

> **Nobody should have to parse English to find out which Attribute was wrong.**

## The problem

TS 104-175 § 8.3.3 requires three members — `type`, `title`, `detail` — and
RFC 7807 adds `status` and `instance`. Only `type` is machine-readable, with
**twelve possible values for the entire API**. Which Attribute, Entity, body
member, registration or rule an error is about is prose in `detail`, whose
content is implementation-defined. So:

- **A client cannot act on it** — fixing or dropping the offending member needs
  to know which one, and string-matching `detail` breaks on the next broker.
- **A test suite cannot assert on it** — it checks "400 BadRequestData", which a
  broker rejecting the body for an unrelated reason also passes.
- **Implementations invent fields anyway** — Coraine emits `attributeName`,
  `registrationId` and `statusCode`; three brokers doing this produce three
  vocabularies, and agreeing one is possible only while the number is small.
- **Prose clause references rot** — Coraine's `"… (§ 4.6.2)"` strings are
  GS CIM 009 V1.9.1 numbers, wrong under TS 104-175, and nothing detects it.

Two spec gaps, each worth a slide: **`Conflict` has no HTTP status** (§ 8.3.2
defines twelve types, TS 104-176 § 6.3.2 maps eleven; Coraine answers 409 by
analogy — spec-doubt #126), and **the cited RFC 7807 is obsoleted by RFC 9457**.

## The constraints

1. ProblemDetails is `application/json`, not JSON-LD — new members are plain
   JSON names, not terms in the core `@context`.
2. Every new member is **optional** to read (RFC 9457 § 3.2: ignore unknown
   members), so no deployed client breaks.
3. Names are **Fully Qualified with the alias beside them**: TS 104-176 § 6.2.3
   allows only FQNs in error bodies, so a name member is an object
   `{ "<alias>": "<FQN>" }`.

## Our position

- **Sub-error codes**: an `errorCode` *beneath* the twelve `type`s, not new
  top-level types — the only shape addable without a breaking change.
- **`invalid` is not a top-level type**: a malformed body is a sub-code of
  `BadRequestData`.
- **Flat, not recursive**: one `problems`/`errors` array; a forwarded error
  carries the `registrationIds` it came through, and an intermediate broker
  appends downstream problems to its own. A stack trace is a flat list, not a
  tree.

What makes optional members worth having is a **conditional obligation**: "when
an error concerns named Attributes, it **shall** include `attributeNames`".
`should` throughout would make it decorative; how hard to hold that line is
still to be decided.

## The proposed member set

| Tier | Members |
|---|---|
| **Always** | `type`, `title`, `detail` (as today), `status` (promoted to mandatory), `errorCode` (the sub-code) |
| **Whenever it applies** | `entityIds`, `attributeNames`, `subAttributeNames`, `entityTypes`, `datasetIds`, `registrationIds`, `subscriptionIds`, `snapshotIds`, `pointer` (JSON Pointer into the request body), `retryable` |
| **Optional** | `instance`, `receivedValue`, `expectedValues`, `retryAfter`, `limit`, `specClause` (document + version + clause), `diagnostics` (one object: broker, version, gitHash, file, line, `requestId`) |

Identifiers are arrays of URIs; names are objects, alias → FQN. Plural
throughout, because one error can be about several things.

```json
{
  "type": "https://uri.etsi.org/ngsi-ld/errors/BadRequestData",
  "title": "Bad Request Data",
  "status": 400,
  "errorCode": "invalidJson",
  "detail": "…",
  "entityIds": [ "urn:ngsi-ld:Vehicle:A" ],
  "attributeNames": { "speed": "https://example.org/vocab#speed" },
  "pointer": "/speed"
}
```

With these members in the ProblemDetails itself, **EntityError** and
**NotUpdatedResult** are no longer needed — both are a ProblemDetails — which is
what lets [error-reporting.md](error-reporting.md) make every partial result one
array of one type.

## What Coraine would change

Little: `ldErrorExtraString` / `ldErrorExtraInt` (`corNgsild/ldError.c`) already
build the extension object; `attributeName`/`registrationId` become plural
arrays; `pointer` needs the parser to keep the path; the `diagnostics` block is
nearly free, since `file`/`line` already reach `ldError`. About a day for an
existing implementation.

The full reference: [problem-details-details.md](problem-details-details.md).
How it came about: [history](history/problem-details.md).
