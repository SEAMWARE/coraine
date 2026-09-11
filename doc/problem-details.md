# Typed ProblemDetails — material for an ice-breaker

Raw material for a **slide deck to open a discussion at Athens, October 2026**.
Not a contribution and not a CR: the aim is to get TC DATA arguing about the
right question, which is *what should an NGSI-LD error be able to say?* — and to
have a menu on the table when they do. Draft 2026-09-11.

The one-line provocation for slide 1:

> **Nobody should have to parse English to find out which Attribute was wrong.**

## 1. What an NGSI-LD error carries today

TS 104-175 § 8.3.3 requires exactly three members:

- **type** — the error-type URI, from the table in § 8.3.2
- **title** — "a short string summarizing the error"
- **detail** — "a detailed message that should convey enough information about
  the error"

IETF RFC 7807 § 3.1 supplies two more that the clause's own EXAMPLE 1 uses:
**status** and **instance**. Five members, of which exactly one — `type` — is
machine-readable, with **twelve possible values for the entire API**.

There is no `errorCode`. Everything below the granularity of those twelve types
— which Attribute, which Entity, which member of the body, which registration,
which rule — is prose inside `detail`, whose content the spec explicitly leaves
to the implementation by saying only that it *should* convey enough information.

Two things found while reading the two clauses side by side, both worth a slide
of their own:

- **`Conflict` has no HTTP status.** § 8.3.2 defines twelve error types;
  TS 104-176 § 6.3.2 maps eleven. The missing one is `errors/Conflict` — which
  clause 10 mandates at **shall** level in 27 places. § 6.3.3 says the status is
  "as per clause 6.3.2 depending on error type", and for this one type clause
  6.3.2 says nothing. Checked against GS CIM 009 V1.9.1: identical gap, so it is
  not a TS 104 conversion artefact. Coraine answers 409 by analogy with
  `AlreadyExists`. That is a guess, and Scorpio and Stellio are presumably
  guessing separately. Filed as spec-doubt #126; a clean standalone CR.
- **The cited RFC is obsolete.** [n.6] is RFC 7807, obsoleted by **RFC 9457**
  (July 2023). 9457 is backwards-compatible and sharpens exactly the part all of
  this rests on — that a problem type may define extension members and consumers
  must ignore unknown ones. Updating the reference is close to a prerequisite.

## 2. Why prose is not enough

### 2.1 A client cannot act on it

The three things a client does after a 400 are: fix the offending member and
resend, drop it and resend, or give up and log. Choosing between them needs to
know *which member*. Today that is recoverable only by string-matching a
`detail` that is explicitly implementation-defined — so the matching breaks on
the next broker, and on the next release of the same broker.

Batch operations are the sharpest case. § 6.14.3.1 gives `errors[]` an entity id
alongside each ProblemDetails, which is precisely an admission that identity has
to be structured to be usable. The admission stops at the entity; below it,
nothing.

### 2.2 A test suite cannot assert on it

The ETSI conformance suite can assert on the HTTP status and on `type`. It
cannot assert that a broker rejected *the right thing*. A TP meaning "the broker
shall reject the Attribute with the malformed name" can only check "the broker
answered 400 BadRequestData" — which a broker rejecting the whole body for an
unrelated reason also passes.

That is a conformance gap, not an ergonomics one: the suite cannot currently
distinguish a right answer from a right status code for the wrong reason.

### 2.3 Implementations are inventing the fields anyway

Coraine already emits three unregistered members, each added when a real user
hit a real error: `attributeName` (String), `registrationId` (String),
`statusCode` (Number).

RFC 7807 § 3.2 permits this, so none of it is non-conformant. But three
implementations doing it independently produce three vocabularies, and the
window in which a common one can still be agreed is open only while the number
of extension members is small. It is small **right now**. That is the slide that
makes the case for having the discussion in Athens rather than in two years.

### 2.4 Prose clause references rot, silently

Coraine's error strings cite the clause a rejection rests on — `"… (§ 4.6.2)"`.
Those are GS CIM 009 V1.9.1 numbers, and the day TS 104-175 publishes they are
**all wrong**: § 4.6.2 is now § 5.2.2.3. Nothing detects it. It is a string;
strings are not renumbered by tools and no test fails.

This is the argument for making the clause reference a **field**, and it is
first-hand: we are sitting on a body of error messages we know to be stale and
cannot mechanically find. A typed field can be validated against the document it
names. A sentence cannot. It is also why the field must carry the **document and
the version**, not just a number — clause numbers move between editions, which
is exactly what just happened to ours.

## 3. Three constraints that rule things out

1. **ProblemDetails is `application/json`, not `application/ld+json`.** § 8.3.3
   is explicit, and gives its reason: the standard JSON MIME type "so that old
   clients or existing tools are not broken". So these are **plain JSON member
   names**, not JSON-LD terms. They do not expand, they do not compact, they are
   not in the core `@context`. Any proposal that quietly makes the error body a
   JSON-LD document breaks the stated reason § 8.3.3 gives for its own rule.
2. **Every new member must be OPTIONAL.** RFC 9457 § 3.2 requires consumers to
   ignore extension members they do not recognise, so adding them cannot break a
   deployed client. A mandatory member would break every deployed broker at
   once. The teeth have to come from a *conditional* obligation instead — § 6.
3. **Names follow the request's `@context`.** An Attribute name in an error
   should be the name as the client wrote it, compacted per the request's
   `@context`, like every other name in a response. Returning an expanded IRI
   where the request said `speed` makes the field harder to use than the prose
   it replaces.

## 4. The menu

Grouped by class, because the classes have genuinely different rules — who reads
them, whether they can be switched off, and whether a test suite can assert on
them. Keeping the groups apart on the slides is probably the single most useful
thing the deck can do.

### 4.A — Which NGSI-LD elements the error is about

*Read by client logic. Testable. The core of the proposal.*

| member | type | notes |
|---|---|---|
| `entityIds` | String[] | |
| `attributeNames` | String[] | compacted per the request `@context` |
| `entityTypes` | String[] | |
| `registrationIds` | String[] | **the one that matters for forwarding** |
| `subscriptionIds` | String[] | |
| `datasetIds` | String[] | where the error is instance-specific |

Arrays throughout, even for the common single-valued case. "These three
Attributes are unknown" is one good error, and forcing it into three round trips
or into prose is the current state. Arrays also stop the vocabulary growing a
singular *and* a plural form of every name later.

`registrationIds` deserves its own slide. When a distributed operation fails, the
registration is the only thing that tells a client *where* it failed, and it is
the only identity in the whole error that the client did not itself send. Prose
is a particularly bad place to keep it.

### 4.B — Which part of the submitted body was wrong

*Read by client logic. Testable.*

| member | type | notes |
|---|---|---|
| `pointer` | String | JSON Pointer (RFC 6901) into the request body |
| `receivedValue` | any | what was actually there |
| `expectedValues` | String[] | the permitted alternatives, where a closed set exists |

**On naming.** "offending field" and "offending value" say it exactly, but
`pointer` / `receivedValue` are worth considering instead:

- `pointer` rather than `offendingField` because it is a *location*, not a name,
  and that is what makes it general. `"/relatedParty/@referredType"` locates the
  fault at any depth, in a body of any shape, and covers members that are not
  Attributes at all — a malformed `observedAt`, a bad batch element (`/3/type`).
  RFC 9457 § 3.2 already suggests a pointer-shaped extension for this, so there
  is precedent to cite rather than invention to defend. It would have pinpointed
  the `@referredType` bug with no NGSI-LD vocabulary whatsoever.
- `receivedValue` rather than `offendingValue` because it pairs naturally with
  `expectedValues`, and "received" is neutral about whose fault it is — which
  matters when the value came from a forwarded source rather than from the
  client. (`offending`, honestly, is also the kind of word that reads as
  charming in English and translates badly in a standards document.)

Two practical rules `receivedValue` needs, or it will not survive review: it must
be **truncated** (a rejected JsonProperty can be megabytes, and an error body
that large is a denial-of-service on the client), and it must **only ever echo
something the request actually contained**.

`expectedValues` is the quiet win — it turns "invalid format" into a list of the
formats this broker accepts, which is the difference between a client that
recovers and one that logs.

### 4.C — Diagnostics

*Read by a human debugging, or by the broker's own developer. **Not** client
logic. Not testable, and should not be.*

| member | type | notes |
|---|---|---|
| `broker` | String | implementation name |
| `version` | String | |
| `gitHash` | String | |
| `file` | String | source file |
| `line` | Number | source line |
| `requestId` | String | correlation id, to match against the broker's log |

**Recommendation: put the whole class inside one member.**

```json
"diagnostics": { "broker": "coraine", "version": "0.4.0",
                 "gitHash": "b0682e3", "file": "ldNormalizeInput.c",
                 "line": 231, "requestId": "01J…" }
```

Three reasons, and they are worth arguing on a slide:

1. A client can tell *at a glance* that the entire block is implementation-
   internal and ignorable. Five loose top-level members look like protocol.
2. A deployment can switch the block off with **one** flag. It will want to:
   source file and line are internals, and there is a fair argument that a
   public-facing broker should not volunteer them.
3. The spec can then say almost nothing about the contents — "implementation-
   defined, consumers shall ignore" — which is much easier to get agreed than
   six individually-registered members, and leaves room for whatever the next
   implementation wants to put there.

`requestId` is the sleeper in this group. On its own it converts every support
conversation from "can you paste the error" into "give me the id", and it is the
one diagnostic that is useful even when the block is otherwise stripped.

### 4.D — What the client should do next

*Speculative — include on the slide as provocations, not proposals.*

| member | type | notes |
|---|---|---|
| `retryable` | Boolean | is resending the identical request ever going to work? |
| `retryAfter` | Number | seconds; pairs with the HTTP header |
| `limit` | Number | for `TooManyResults` / `TooComplexQuery` — "you asked for 50 000, the maximum is 1 000" |

`retryable` looks trivial and is not: a 504 `LdContextNotAvailable` is retryable,
a 400 never is, and a distributed 207 is retryable *for some of the entities*.
Getting a room to agree on which is which would be a productive argument.

### 4.E — Which rule was broken

*This is the one you actually asked for.*

| member | type | notes |
|---|---|---|
| `specClause` | String[] | the clause(s) the rejection rests on |

The hard part is the grammar, because `"5.2.2.3"` alone is ambiguous across three
documents (TS 104-175, -176, -243) and meaningless across editions. Three
candidates:

1. **`"TS 104-175 V1.1.1 5.2.2.3"`** — document, version, clause; space
   separated. Greppable, obvious to a human reading a log, parseable with a
   split.
2. **An object** — `{"document": …, "version": …, "clause": …}`. Unambiguous,
   heavier than anything else in a ProblemDetails.
3. **A URI into the published document** — clickable, but ties the field to
   ETSI's hosting and to fragment anchors that are not stable.

Preference: (1), grammar normatively specified, **version mandatory**. The
renumbering in § 2.4 is the exact failure mode this field exists to prevent, and
a version-less reference reintroduces it on day one.

An array, because a rejection can rest on two clauses at once: `@referredType`
sits on § 5.2.2.3 (the name grammar) *and* on the JSON-LD keyword rule.

## 5. Error stacking — the one to think hardest about

RFC 9457 lets us nest ProblemDetails inside ProblemDetails, so "should it be
recursive?" is a real question and not a rhetorical one. **Recommendation: no —
flatten, and carry provenance per element.**

The instinct to nest comes from conflating two different shapes:

- **Siblings** — several independent things wrong with one request (three bad
  Attributes). That is a *list*, and nesting buys nothing.
- **A causal chain** — "I failed because the source I forwarded to failed". That
  is where nesting is tempting, and it is the distributed case.

The chain is real, but it is a chain, not a tree, and a chain flattens without
loss **provided each element says where it came from**. The analogy to use on the
slide: a **stack trace is a flat list, not a tree**, and nobody has ever wanted
it otherwise. Each frame carries its own provenance; the order carries the
causality.

```json
"problems": [
  { "type": "…/BadRequestData", "title": "…", "detail": "…",
    "registrationIds": ["urn:ngsi-ld:csr:7"], "pointer": "/speed" }
]
```

One flat array. Elements carry `registrationIds` where they came from a forwarded
request, and nothing where they came from the local broker. The topology is
reconstructible without a single level of nesting.

What recursion would force the spec to answer, and what will sink it in the room
if it is proposed:

- **Maximum depth?** Unbounded means unbounded response size, from a chain of
  brokers none of which can see the whole chain.
- **Loop detection?** A→B→A is possible in a distributed deployment and nothing
  in the model prevents it.
- **What does a client do with depth 4?** Realistically it reads the leaf and
  ignores the rest, which is an argument that the intermediate levels were never
  earning their place.
- **How does the test suite assert on a tree?** It effectively cannot.

Flat also lets one member serve both shapes — siblings and chain are the same
array — which is one fewer thing to name and one fewer thing to argue about.

The rule to propose: **an intermediate broker flattens.** It appends the
downstream problems to its own array, stamping each with the registration it came
from. Depth never exceeds one at any hop, the response stays bounded, and the
client still learns exactly which source refused what.

## 6. Making any of it worth having

Optional members an implementation may always omit are worth nothing to a test
suite. The teeth have to be a **conditional obligation**:

> When an implementation reports an error that concerns one or more named
> Attributes, it **shall** include `attributeNames`.

…and the same shape for the rest of group 4.A and 4.B. Enforceable, testable,
costs a conforming implementation nothing it does not already know, and touches
no client that never reads the field. Groups 4.C and 4.D stay `may`, because
they are diagnostics and advice rather than facts about the request.

The weaker alternative — `should` throughout — makes the whole thing decorative.
Worth deciding before the meeting how hard to hold that line, because it is the
one point where the discussion will push back.

## 7. What Coraine would change

Small, and mostly already there:

- `ldErrorExtraString` / `ldErrorExtraInt` (`corNgsild/ldError.c`) already build
  the extension object. The mechanism exists; only the vocabulary is missing.
- `attributeName` → `attributeNames`, `registrationId` → `registrationIds`, as
  arrays. Two call sites.
- `statusCode` is ours and has no equivalent above. It belongs to distributed
  operations — the status a forwarded request came back with — and is worth
  raising separately rather than smuggling in.
- `pointer` is new; needs the parser to keep the path to the offending node.
  Cheap on the write path, where the walk is already recursive.
- The 4.C block is nearly free — `file`/`line` are already passed to `ldError`
  for the log (that is what the macro's `__FILE__`/`__LINE__` are for) and are
  simply not rendered.
- `specClause` is new, and adopting it is what would finally make our stale
  `§ 4.6.2` references mechanically findable.

Worth being able to say in the room: **the cost to an existing implementation is
about a day.**

## 8. Open questions

- **`instance`.** RFC 7807 already has it, and § 8.3.3's example puts an Entity
  id there. Does `entityIds` duplicate it, or does `instance` keep its RFC
  meaning (a URI identifying the *occurrence*) with `entityIds` carrying NGSI-LD
  identity? The § 8.3.3 example arguably uses `instance` wrongly, which is worth
  settling either way.
- **Batch responses.** `errors[]` pairs an entity id with a ProblemDetails. If
  ProblemDetails grows `entityIds`, the pairing is redundant — and if § 5's flat
  `problems[]` lands, `errors[]` is arguably a special case of it. Unify, or
  leave batch alone?
- **How many is too many?** Everything added now can never be removed. Group 4.A
  may already be two too many: a case can be made that `pointer` + `entityIds`
  covers everything real and the rest is convenience. Worth deciding what the
  *minimum defensible set* is before arguing for the comfortable one — the room
  will attack the longest list, and it is better to have chosen the short one
  deliberately.
- **Does the deck propose, or only ask?** An ice-breaker that arrives with a
  finished vocabulary invites a fight about the vocabulary. One that arrives with
  § 2 (the problem), the stack-trace analogy, and a menu invites a discussion
  about what belongs on it. The second is likelier to end with someone else
  volunteering to co-author the CR.
