# Tutorial doubts

Errors and questionable content found in the **FIWARE tutorials** while running
them against coraine. The companion to `spec-doubts` and `testsuite-doubts`: one
entry per problem, numbered, with what the tutorial says, what actually happens,
and which side is wrong.

⚠️ **Not published** — excluded from the site in `mkdocs.yml`. These are notes
about someone else's material, to be raised with the author, not broadcast.

**Classification.** Every divergence is one of:

- **T** — tutorial error (its text, payload or config is wrong)
- **C** — coraine bug (tracked separately; listed here only as a cross-reference)
- **D** — deliberate difference, or spec-silent, needing a decision

The tutorials were written against another broker's behaviour, so that baseline
is not automatically the spec.

---

## T1. `Getting-Started` documents a broker-specific `/version` payload as *the* response

**Where:** `tutorials.Getting-Started`, "Checking the service health".

The README shows the response as:

```json
{ "orion version": "...", "orionld version": "...", "compile_time": "...",
  "compiled_by": "...", "compiled_in": "...", "release_date": "...",
  "git_hash": "...", "doc": "...", "uptime": "..." }
```

**`GET /version` is not an NGSI-LD endpoint.** It is whatever the broker chooses
to emit, so a tutorial offering a choice of four brokers cannot show one broker's
payload as the expected output. coraine answers:

```json
{ "product": "coraine", "version": "0.4.0", "stack": { "<lib>": "<sha>", ... } }
```

Both are fine; the README is wrong to assert either. Suggest showing only that a
JSON document comes back, or one example per broker.

## T2. Every tutorial pins `CORAINE_VERSION=0.4.0`

**Where:** `.env` in all 16 coraine-enabled tutorials.

0.4.0 is a fixed release. Anyone following the tutorials today runs a build that
predates weeks of fixes — including the one where `CORAINE_DATABASE`,
`CORAINE_TROE` and `CORAINE_APIPLUGINS` were ignored, which matters because the
tutorials configure by environment.

Needs either a rolling tag to track, or a periodic bump. ⚖️ Not `:latest` —
an explicit dated tag is reproducible when a report comes in.

## T3. `ngsi-context.jsonld` pins core `@context` **v1.8**

**Where:** `data-models/ngsi-context.jsonld`, most tutorials.

```json
{"@context": ["http://context/user-context.jsonld",
              "https://uri.etsi.org/ngsi-ld/v1/ngsi-ld-core-context-v1.8.jsonld"]}
```

A broker running a different core version cannot honour that element: coraine
runs v1.9 and flags any non-current core URL `ignored` — stub inserted, mappings
skipped — because older variants would fight the current core for ownership of
`value`, `object` and friends.

Harmless in practice (the user terms are in the first element, and the broker
supplies its own core), but it is a version claim the tutorial cannot make good
on, and it will silently mean something different on each broker. Suggest
dropping the core element and letting the broker add its own.

**D** rather than **T** if the group decides a client naming a core version is
legitimate — that question is open with ETSI TC DATA.


## D4. `Getting-Started` geo-query response omits coraine's `geoDistance`

**Where:** `tutorials.Getting-Started`, "Filter context data by geographic
location" (`georel=near;maxDistance==800`).

The README's expected response has `address`, `category`, `id`, `location`,
`name`, `type`. coraine additionally returns:

```json
"geoDistance": 535.274158845
```

**This is a deliberate coraine extension, not a defect on either side.** A
`near` query annotates each match with the distance in metres from the query
point — a non-reified plain number, only on near queries, surviving the
keyValues representation. It is not in TS 104-175 yet; the placement at entity
top level is provisional, pending ETSI standardising the field.
`geoproperty_near_geodistance.test` specifies and defends it.

⛔ **I initially classified this as a coraine bug and removed the member.** That
broke 8 corDB and 9 mongoc tests, which were correctly defending the feature.
The tutorial cannot be expected to show a broker-specific extension, and coraine
will not drop one to match a tutorial — so this is a **D**, recorded so nobody
"fixes" it again.

Worth raising with the author only as a note: a tutorial offering four brokers
may want to say that extra members can appear.


## T5/D5. `CRUD-Operations` queries entities by `id` alone — which coraine rejects

**Where:** `tutorials.CRUD-Operations`, "Read multiple attributes from multiple
entities".

```
GET /ngsi-ld/v1/entities/?id=urn:ngsi-ld:TemperatureSensor:001,urn:ngsi-ld:TemperatureSensor:002
    &format=simplified&pick=id,type,temperature
```

coraine answers **400 BadRequestData**:

> Query Entities requires at least one of 'type', 'attrs', 'q', a GeoQuery,
> 'scopeQ', or 'local=true' (§ 5.7.2.4 — id / idPattern alone is too wide)

That is deliberate and tested — `query_entities_too_wide.test` step 03 asserts
`GET /entities?id=urn:V1` is 400, citing § 5.7.2.4.

❓ **OPEN, and it is a spec question, not a tutorial one.** An explicit list of
entity ids is the NARROWEST possible query — narrower than `type=`, which we do
accept. Rejecting it as "too wide" reads oddly, and the tutorial (written against
another broker) clearly expects it to work. If our reading of § 5.7.2.4 is wrong
then this is a coraine bug and a fairly visible one, since querying a known set
of ids is an ordinary thing to want.

⚠️ coraine contradicts itself in writing on this. `getEntities.c:1583` says:

> *"The spec lists type/attrs/q/georel/local; we additionally accept id and
> idPattern since they bound the candidate set as tightly (an explicit URI list
> is not a 'too wide' query)."*

…and its check includes `hasId`. But `ldParamsValidate.c` runs first, for GET,
and rejects `id`-only — so that branch is unreachable on the GET path and its
comment describes behaviour the broker does not have. Whichever way the spec
question lands, the two should agree. (No behavioural split, though: the POST
`/entityOperations/query` variant also refuses, demanding `type` in the body.)



## D6. `Short-Term-History` and `Time-Series-Data` drive Mintaka, which cannot run on coraine

**Where:** both tutorials' temporal sections.

Mintaka is a separate temporal-query service that reads **another broker's TRoE
Postgres schema directly**. coraine has its own TRoE and implements the temporal
API itself, so Mintaka cannot be pointed at it — the schema is not shared. KZ:
*"Mintaka can't run on coraine's DB, so forget about Mintaka."*

Historical reason, per KZ: six months ago the other broker persisted temporal
data but did not implement temporal QUERIES; Mintaka did that half. coraine does
both, so the split does not apply to it.

⭐ **Worth knowing before skipping these two outright.** Mintaka serves the same
NGSI-LD temporal API, one base path down:

    Mintaka   http://localhost:8080/temporal/entities/{id}?lastN=3&format=temporalValues
    coraine   http://localhost:1026/ngsi-ld/v1/temporal/entities/{id}?lastN=3&format=temporalValues

Short-Term-History has 10 steps at Mintaka and 1 at the broker, and the queries
themselves — `lastN`, `pick`, `format=temporalValues`, `q` filtering, and the
"without observedAt" section — are ordinary NGSI-LD temporal queries. Redirected
at coraine's own temporal endpoint they would exercise a real-world set of
temporal queries that only our own functests cover today.

⇒ **DECIDED: test them, with the queries redirected at coraine.** KZ: *"must
test but the history queries go to coraine, not mintaka"*, and *"it's all
NGSI-LD, going from one broker to another should be quite painless"*. The runner
rewrites the base path; `/info` is Mintaka's own health endpoint and is dropped.

⭐ And the prerequisite is already in place: `Short-Term-History`'s
`docker-compose/coraine.yml` configures `--troe timescale --troeHost
timescale-db --troeName corh` with a `timescale-db` service of its own. The
tutorial author set coraine up for temporal properly - only the query URLs were
left pointing at Mintaka.

⚠️ **QuantumLeap is a different case and should NOT be skipped with it.** Both
tutorials also reference QuantumLeap, which receives NGSI **notifications**
rather than reading a schema — so it works with any broker that notifies
correctly, and testing it exercises coraine's subscription path.

---

# Phase 1 sweep — 2026-09-18 (re-run, verified)

All 16 coraine-enabled tutorials were driven against `coraine:local`, no fixes
applied. **168 steps across 11 tutorials, 18 flagged.** Five stacks never
finished starting — see T10.

The first sweep of the day is **withdrawn**: its extractor kept only requests to
the broker (`:1026`) and dropped everything aimed at the IoT Agent (`:4041`),
the device simulator (`:7896`) and the tutorial app (`:3000`). The IoT tutorials
interleave agent provisioning with broker queries, so the broker was being asked
for entities that had never been created; it answered `NonexistentTenant`,
correctly, and I recorded three "flags" per IoT tutorial that were my own doing.
`extract.py` and `compare.py` now apply the identical port filter — they must,
or the step numbering diverges and every request/response pairing shifts by one.
`IoT-Agent` went from 7 steps to 27 and from 0 provisioned devices to 5.

The 18 flags map onto T7-T14 plus C1 below; `Getting-Started`'s single flag is
**T1**, already written up above. Two further findings (D15, D16) came from the
broker logs and from no flag at all.

Every entry below was then **reproduced by hand** against a broker started for
the purpose. Clean: **CRUD-Operations** (22 steps), **Merge-Patch-Put** (24) and
**IoT-Sensors** (one step; pure device simulation).

## C1. ⭐ A JSON object inside a Property's `value` is REWRITTEN on retrieval

**The one coraine bug the sweep found, and it is a real one.**

Seen in `IoT-Agent` step 7, where a Device came back as

```json
"category": { "type": "Property", "value": { "@type": "VocabProperty", "@value": "sensor" } }
```

§ 5.2.2.5 is unusually direct about this - implementations **"shall preserve the
representation of the content of the values provided by the context information
providers and return the original content when replying to context consumption
requests"**. A `shall`, not a `should` (TS 104-175 § 5.2.2.5, the clause CIM 009
numbered 4.6.4).

⭐ **"Opaque" is not the right word, and the distinction is the whole bug** (KZ,
2026-09-18). A value is not untouched: it is JSON-LD expanded on the way in and
compacted on the way out, and it may be escaped and unescaped at a storage
boundary. Those are **round trips** - what comes back is what went in. What a
value must never be is **read as NGSI-LD structure**, because reinterpreting it
is not reversible: there is no way back from a renamed key.

⚠️ **The tell was not this bug** - settled, see C4. `{"@type":"VocabProperty",
"@value":"sensor"}` round-trips through coraine verbatim, so step 7's output is
what the IoT AGENT sent, faithfully returned. C1 below is a different shape (a
renamed key) and is reproduced and fixed on its own evidence, independently of
the tutorial. The tutorial is what made me look.

Which is what happened:

| stored inside `value` | returned |
|---|---|
| `{"type":"VocabProperty","value":"sensor"}` | `{"type":"VocabProperty","vocab":"sensor"}` |
| `{"type":"LanguageProperty","value":"x"}` | `{"type":"LanguageProperty","languageMap":"x"}` |
| `{"type":"ListProperty","value":["a"]}` | `{"type":"ListProperty","valueList":["a"]}` |
| `{"type":"JsonProperty","value":{"k":1}}` | `{"type":"JsonProperty","json":{"k":1}}` |
| `{"type":"Property","value":"sensor"}` | unchanged |
| `{"type":"Relationship","object":"urn:a"}` | unchanged |
| `{"type":"Foo","value":"bar"}` | unchanged |

**Cause.** `restoreValueKey()` in `corNgsild/ldEntityToApi.c` renames the stored
`value` key back to the type's own IRI (`hasVocab`, `hasLanguageMap`, …) — which
is right for an attribute — and then **recurses into every child object that has
a `type` naming a known attribute type**, to reach sub-attributes. A Property's
value node satisfies that test whenever the user's data happens to carry a
`type` member, so user data is walked as if it were NGSI-LD structure.

The last three rows are why it stayed hidden: `Property` renames `value` to
`hasValue`, which compacts straight back to `value` — invisible. Only the types
whose value key is spelled differently show the damage.

**Two sites, same shape.** `timestampsToIsoStrings()` in the same file recurses
by the same test, and its comment even claims to exclude "an opaque value
object" while doing no such thing. An integer the user called `observedAt` or
`createdAt` inside a value is rewritten as an ISO string - `42` comes back as
`"1970-01-01T00:00:00Z"`.

**Fix shape:** the sub-attribute recursion must skip the value node itself. The
same two-oracle shape as the `@referredType` bug — classify by NAME in one place
and by POSITION in another, and the two disagree exactly where a user puts NGSI-LD
vocabulary in their own data.

⚠️ Reproduced deliberately, single broker, `--database corDB`, no tutorial stack.

## C5. ⛔⭐ OPEN, and the worst of the four - an ALREADY-EXPANDED core term is not recognised

KZ, 2026-09-18: *"I talk about incoming already expanded core context terms. As
we don't expand those, if they come in expanded, that would be a problem."* It
is.

**And the spec does not forbid the long form** (checked, KZ asked). § 4.3.4.2 is
permissive - operations *"**allow** clients to use short-hand strings as
non-qualified names"* - and expansion *"is performed using an @context as
described by the JSON-LD specification, clause 5.1"*, under which a term and its
IRI land on the same IRI, so the two inputs are indistinguishable after step 1.
The "reserved" language in § 5.2.3 / § 5.2.5 is about users not REDEFINING core
terms in their own @context, not about what a client may send. Clause 7 says
*"the use of short names is recommended"* - a recommendation, about the query
language. There is no `shall not` tying names to the short form anywhere.

⇒ Short names are what the spec expects and what responses must use where the
@context allows; the long form is legal input that must expand to the same
thing. This is a plain conformance bug, not an unsupported extra. After JSON-LD expansion a term and its IRI are the same thing, so a client
that sends `https://uri.etsi.org/ngsi-ld/observedAt` has sent `observedAt`. We
do not read it that way.

The `@vocab` and GeoJSON branches are fine. It is specifically the
**core-context structural terms**:

| sent EXPANDED | what happens |
|---|---|
| `…/default-context/T` as the entity type | ✅ `T` |
| `…/default-context/P` as an attribute name | ✅ `P` |
| `https://purl.org/geojson/vocab#Point` | ✅ `Point` |
| `https://uri.etsi.org/ngsi-ld/Property` as an attribute `type` | ⛔ **two `type` members in ONE object** |
| `…/ngsi-ld/observedAt`, `unitCode`, `datasetId` | ⛔ stored as USER Properties, normalized - attribute AND sub-attribute level |
| `…/ngsi-ld/hasValue`, `…/hasObject` | ⛔ `400`, the attribute has no recognised value key |

The first row emits **invalid JSON**:

```json
"P":{"type":"Property","type":"https://uri.etsi.org/ngsi-ld/Property","value":1}
```

A strict parser takes the last member, so the client is told the attribute's type
is the IRI; others reject the document outright. Verified byte-for-byte with
`od -c` - it is two members, not a rendering artefact.

The second row is the one KZ predicted: the structural sub-attribute stops being
the special `observedAt` and is normalized into
`{"type":"Property","value":"2026-01-01T00:00:00Z"}`. The timestamp is gone.

**Where, and why the obvious fix was pulled once before.**
`corNgsild/ldNormalizeInput.c` carries the assumption in a comment:

> *"The attribute-type keywords above are core-context terms and are NEVER
> JSON-LD-expanded - they always arrive in short form."*

and records that an earlier broad `starts with https://uri.etsi.org/ngsi-ld/`
check was removed because it also matched the `@vocab` expansions - a `Poinxt`
geometry arriving as `…/default-context/Poinxt` was stored as junk instead of
being rejected. ⭐ That does not rule the fix out, it rules out the *prefix
test*: an **exact** match on `https://uri.etsi.org/ngsi-ld/<Term>` cannot collide
with `…/default-context/<anything>`.

**Fix shape.** An input key that is already an absolute IRI should be compacted
against the **core** context - which is what already happens for the
default-context names, and simply does not for `ngsi-ld:<term>`. The term set is
closed and small: `LdVocab.h`'s structural members plus the eight attribute-type
names, and the `KJF_CORE_TERM | KJF_ATTR_TERM` stamping machinery already exists
(`addTypeField` does it by hand). ⚠️ It must not reach inside a Property value -
same opacity question as C1 and C4.

⚠️ Independently of all that: **nothing should ever emit two members with the
same name into one object.** That guard belongs lower down, and is worth having
whatever is decided here.

## C7. ⭐⭐ `coreContextRewriteToShort` has FOUR victims, not one - the destroyed core context

KZ, 2026-09-18: *"We might need a second copy of the core context for this ... as
we've deliberately 'destroyed' the real core context."* That is the right
diagnosis, and by now there is enough evidence to size it.

`coreContextRewriteToShort` sets `itemP->id = itemP->name` for every core term,
so the expander returns short forms for core terms with zero per-call work. It
also means **the core context no longer knows any of its own IRIs**, and four
separate places need them:

| consumer | symptom | state |
|---|---|---|
| reverse lookup on input (C5) | `valueCompare` reads `itemP->id` at LOOKUP time, so an IRI is compared against a short name and never matches - the long spelling of a core term was unrecognised | fixed with an ad-hoc snapshot, `coreIriHT` |
| `corLdCompact` step 3 | same cause; the step is **dead code** for the core context, so a core IRI never compacts back | OPEN, deliberately untouched (changes wire shapes) |
| `corLdPrefixExpand` | concatenates `prefixItemP->id` + suffix, which is now the prefix NAME. `ngsi-ld:speed` was stored as an attribute called **`ngsi-ldspeed`**, with a `201` | fixed by consulting `corLdCorePrefixes()` |
| the prefix snapshot itself | exists ONLY because the rewrite destroys the data - `corePrefixV`, captured pre-rewrite | pre-existing workaround |

So there are already **two ad-hoc pre-rewrite snapshots** (`corePrefixV` and my
`coreIriHT`) plus one dead code path, all working around the same deliberate act.
A third snapshot would be the wrong answer.

⭐ **KZ's suggestion is the structural one**: keep a second, PRISTINE parse of the
core context beside the rewritten one. The rewritten copy stays exactly as it is
- it is a legitimate optimisation for the hot expand path - and anything needing
a real IRI asks the pristine copy. That replaces both snapshots, revives
`corLdCompact` step 3 as a normal lookup, and removes the trap that has now bitten
three times in one file family.

⚠️ Cost: one more parse of the core context at init (once, ~100 terms) and the
memory for it. Nothing per-request. Against that, every future reader of
`itemP->id` on the core context is a latent bug of exactly this shape.

### Also found, and left alone

`geojson:Point` as a geometry type inside a GeoProperty's value is still refused,
while the fully-expanded `https://purl.org/geojson/vocab#Point` is accepted. That
one is NOT a prefix bug - nothing prefix-expands inside a value, correctly,
because a value is the user's. It is the same question C4 asks: a GeoProperty's
value is broker-owned structure while a Property's is not. Whatever settles C4
settles this.

## C6. ⛔ OPEN - an INVALID attribute type is accepted, and gets a second type bolted on

Found by KZ asking the obvious question about C5: *"That longname is neither of
them, so it should have been an error. And even weirder, add another type field?
What if we have another invalid attribute type? Say `"type": "Porpetry"`."* It
does exactly that:

| sent | response | stored |
|---|---|---|
| `"P":{"type":"Porpetry","value":1}` | **201** | `{"type":"Property","type":"Porpetry","value":1}` |
| `"P":{"type":"Banana","value":1}` | **201** | `{"type":"Property","type":"Banana","value":1}` |
| `"P":{"type":42,"value":1}` | **201** | `{"type":"Property","type":42,"value":1}` |

Two members with the same name in one object - invalid JSON - and a silent
`201` to a request that was wrong. C5's expanded IRI was one INSTANCE of this;
fixing recognition (corNgsild #25) removes that instance and leaves the class.

**Cause.** `normalizeAttr`'s Case 1 asks "is there a `type` naming a known
attribute type?". On `false`, Case 2 fires because the object *has* a `value`
key, infers `Property` and prepends it - never looking at the `type` already
there. "Unrecognised" is treated as "absent".

**Why it cannot simply reject.** Concise GeoProperty is legal and its `type` is
legitimately not an attribute type:

```
"location": { "type": "Point",  "coordinates": [1,2] }   ->  GeoProperty   ✅
"location": { "type": "Poinxt", "coordinates": [1,2] }   ->  400           ✅
```

Case 3 catches a misspelled GEOMETRY because it has `coordinates`. A misspelled
ATTRIBUTE TYPE has none, so nothing trips.

**The rule should be three-way, not two:**
1. no `type` member → simplified/concise → infer
2. `type` names an attribute type, either spelling → normalized
3. `type` present but neither → legal only as the GeoJSON-geometry shape (Case 3
   owns it, and already rejects a bad one); otherwise **400**

⭐ Plus the invariant that would have made every one of C5 and C6 loud instead of
silent: **`addTypeField` must never add a `type` to an object that already has
one.** Nothing should be able to emit two members with the same name.

⚠️ **Not a one-liner, which is why it is its own item.** `ldNormalizeInput`
cannot currently fail - `void`, and not one `ldError` call in the file - and it
has six callers, FOUR of them batch paths where a failure has to become a
per-entity error inside a 207 rather than a request-level 400. `ldCheckAttribute`
will not catch it either: it treats `attrType == LdAttrNone` as a partial update
and accepts it deliberately (§ 5.6.x fragments).

## C4. OPEN - the compaction walk also rewrites keys inside a Property's value

Found by probing KZ's question *"@type ... that's already a fully qualified name,
right? Cause, you might expand that with @vocab"*. We do not expand there. We
**strip** there, which is the mirror image and equally not a round trip:

| in, inside a Property `value` | out |
|---|---|
| `{"https://uri.etsi.org/ngsi-ld/default-context/foo": 1}` | `{"foo": 1}` |
| `{"foo": 1}` | `{"foo": 1}` |
| `{"observedAt": "x"}` | `{"observedAt": "x"}` |
| `{"https://uri.etsi.org/ngsi-ld/observedAt": 1}` | unchanged |
| `{"@type":"VocabProperty","@value":"sensor"}` | unchanged |

Keys inside a value are **not expanded on the way in** - `observedAt` stays
short, it never becomes `ngsi-ld:observedAt` - but the `@vocab` prefix **is**
stripped on the way out. Asymmetric, so § 5.2.2.5's *return the original
content* is broken for any user key that happens to sit under the default-context
prefix.

**Where:** `corJsonld/corLdCompactTree.c`, `compactObject()` recurses into every
sub-object, with an `opaqueKeys` escape only for `@container`-marked terms. A
Property's value node is not one, so the walk compacts the user's own keys.

⛔ **For a JsonProperty the spec removes the judgement.** Clause 5 defines `json`
as *"Raw unexpandable JSON which **shall not be interpreted as JSON-LD** using
the supplied @context"*. We do interpret it:

```
in : "json": {"https://uri.etsi.org/ngsi-ld/default-context/foo": 1, "type": "Point"}
out: "json": {"foo": 1, "type": "Point"}
```

So C4 has a hard case and a soft one: `json` breaches a `shall not` outright,
while a Property `value` rests on § 5.2.2.5's *return the original content*. The
`json` case also settles the layering argument in (b)'s favour - whatever marks a
subtree unexpandable is NGSI-LD knowledge, and `json` is the member the spec
itself names as carrying it.

⛔ **Not a one-line fix, and the reason is worth recording.** Skipping the value
node outright would break GeoProperty: `type: "Point"` is stored expanded to
`https://purl.org/geojson/vocab#Point` (`LD_VOCAB_GEO_POINT`) and needs that
same walk to come back as `Point`. A VocabProperty's `vocab` is stored
`@vocab`-expanded too (`ldEntityMerge.c:347`). So opacity depends on the
ATTRIBUTE TYPE - Property and JsonProperty values are the user's, GeoProperty
values and `vocab` are the broker's own structure - and `compactObject` lives in
**corJsonld**, which has no notion of a GeoProperty. It currently approximates
with two special cases (`type` at level 0, `@container` terms).

Two shapes, KZ's call:
- **(a)** an NGSI-LD-aware opacity rule inside the compactor - cheap, but pushes
  NGSI-LD semantics into the JSON-LD layer that has so far stayed clear of it;
- **(b)** corNgsild marks the opaque subtrees before calling it - keeps the
  layering, and the marker generalises: it is the same thing C1's two walks
  would have wanted.

⭐ **It also settles the C1 caveat.** `{"@type":"VocabProperty","@value":"sensor"}`
round-trips verbatim, so `IoT-Agent` step 7's output is what the AGENT SENT and
coraine returned it faithfully - not C1, not a coraine bug. (`@type` and `@id`
are the only two keyword aliases in the core context; `value` maps to
`ngsi-ld:hasValue`, not `@value`.)

## C3. Found while fixing C1 - kjson chopped any integer of more than 15 digits

Not a tutorial finding, recorded here because it came out of the same thread and
would otherwise be lost. Writing the C1 functest with a nanosecond timestamp in
a value turned up a second, unrelated defect:

```
9007199254740993     ->  900719925474099
1700000000000000000  ->  170000000000000
```

`pushInt` in `kjson/kjRender.c` rendered into a `char number[16]`, so `snprintf`
truncated at 15 characters, and the guard `if (nLen > 15) nLen = 15` then clamped
the copy to match - dressing a truncation as a decision. Digits chopped, no
error, every render path. A signed 64-bit integer needs 20 characters plus the
NUL. `renderedSize2` always measured with `%lld` into `char cv[64]`, so the
estimate was right and the render simply under-filled it: no overrun, pure data
loss.

Fixed in **kjson 0.14.1**, pinned via `corLibs/klib-pins`, functest
`value_large_integer` (15 digits, 16, 19, and both int64 extremes, on both the
normalized and the simplified render path).

⚠️ **And a trap that cost real time, twice in one day.** Verifying C1 against
unfixed code, the test PASSED - which looked like the bug was never there. It
was: `git checkout HEAD~1 -- <file>` does not always leave the source strictly
newer than its `.o`, so `make di` skipped the rebuild and I probed the FIXED
archive while believing it was the unfixed one. `make ci` settled it in one
step. The first reproduction of C1 was suspect for the mirror-image reason - a
`libcorNgsild.a` lying in the tree from an older build. **Clean-build before
concluding either way, and verify the artifact, never the build's exit code.**

## T7. `Entity-Relationships` step 7 — the README abridges its own `pick`

The request asks `pick=id,type,controlledAsset`; coraine returns exactly those
three. The README's expected block shows `[id, type]`. coraine is right and the
README is abridged — **no action on our side**.

The same thing in `Context-Providers` step 8: `pick=id,type,name,comment`
returns all four, the README shows three.

## T8. `Context-Providers` — three separate tutorial errors, no coraine gap

**Withdrawn: the `management` finding.** Step 4's expected block carries
`"management": {"timeout": 1000}`, but the registration the tutorial actually
POSTs at step 2 has no `management` member. The README's expected output belongs
to a different registration (there is one with `management` further down the
page). coraine stores and echoes `management` correctly — verified by hand.

**⛔ Steps 12, 13 and 15 never ran.** The README writes

```bash
curl -G -X 'http://localhost:1026/ngsi-ld/v1/entities/?type=AgriParcel' \
```

`-X` takes the URL as the HTTP METHOD, leaving no URL at all: `curl: (2) no URL
specified`. Three occurrences on one page. It should be `-X GET` followed by the
quoted URL.

**Step 11 is a false flag of mine** — a `PATCH` answering `204 No Content` has no
body to parse, and the comparator counted "no parseable response" as a
divergence.

## T9. `Subscriptions` — the tutorial percent-encodes quotes in a JSON BODY

Steps 1, 2 and 3 all POST a subscription whose `q` reads

```
filling>0.6;filling<0.8;controlledAsset==%22urn:ngsi-ld:Building:farm001%22
```

`%22` is a percent-encoded `"`. Percent-decoding belongs to the URL layer, and
this `q` is in a **request body**, where nothing decodes it — so the value is
literally `%22urn:...%22`, which is not a quoted string and not a URI. coraine
answers `400 Invalid q parameter` naming the offending token.

Both repairs are accepted by coraine, verified by hand:
`controlledAsset=="urn:ngsi-ld:Building:farm001"` and, since the grammar admits a
bare URI, `controlledAsset==urn:ngsi-ld:Building:farm001`.

⚠️ **Worth raising with the author as a portability point, not just a typo**: the
tutorial works on a broker that percent-decodes `q` inside bodies. Coraine does
not, deliberately — see the comment in `ldQParse.c`, where the HTTP layer owns
percent-decoding and the q grammar never sees it.

**Step 3 is a second, independent error**: `"format": "x-ngsiv2-normalized"` is
an Orion-LD extension - the ability to send a notification in NGSIv2 format
rather than NGSI-LD - so the tutorial is asking for something outside the API. § 5.2.12 allows `normalized`, `concise`,
`simplified` and `keyValues`; coraine lists exactly those in its 400.

**Step 7 is a 404 and should be** — it PATCHes
`urn:ngsi-ld:Subscription:5fd228838b9b83697b855a72`, an id from the author's own
machine that no step of the tutorial ever creates.

## T11. `Extended-Properties` step 11 — the README's IRI is not the one it ships

Step 11 queries `type=https://uri.fiware.org/ns/dataModels%23Building` without a
`Link` header and gets `[]`. The `user-context.jsonld` the tutorial ships expands
`Building` to `https://smartdatamodels.org/dataModel.Building/Building`. The
README's text still carries the older `uri.fiware.org` IRI, so it names a type no
entity has.

coraine handles a fully-qualified, `%23`-encoded IRI as a `type` value correctly
— verified both encodings by hand.

## T12. `Big-Data-Flink` step 2 — a blank line inside a curl continuation

```bash
curl -X GET 'http://localhost:1026/ngsi-ld/v1/subscriptions/' \
-H 'Link: ...' \
                               <- blank line
-H 'NGSILD-Tenant: openiot'
```

The backslash continuation is broken by the empty line, so the request goes to
the **default tenant** (`[]`) and the shell reports `-H: command not found`.

## T13. `IoT-Agent` / `IoT-Agent-JSON` step 24 — the agent's own payload moved on

The flag is on `:4041`, not the broker: the README expects a provisioned device
to carry `attributes` (and `explicitAttrs` in the JSON variant); the agent that
ships in the compose file returns `polling` instead. An IoT-Agent version drift
in the tutorial's own text.

## T14. OPEN — `IoT-Agent` step 11, a command device with no attributes

`GET /entities/urn:ngsi-ld:Device:water001` returns `{"id":…,"type":"Water"}` and
nothing else; the README expects the static attributes and the command pairs
(`on_status`, `on_info`, …). `entity_type: "Water"` is correct — that is what the
provisioning asks for — so the type is not the problem; the missing attributes
are.

Not concluded, but the broker log narrows it usefully: `IoT-Agent` logged
**nothing at all** - zero `E:`/`X:` lines - so coraine refused nothing. Whatever
is missing was either never sent, or was asked for through a registration whose
forward came back empty.

One candidate is already out: the tutorial's `coraine.yml` does
pass `--distributed`, so this is not the "registered but never forwarded" shape.
(Verified separately, since it is a useful diagnostic in its own right: with
`--distributed` off coraine accepts a registration with `201` and then forwards
nothing - a query answers `[]` in 6 ms without so much as a connection attempt.)

What is left for phase 3, with the broker log captured: whether the agent ever
sent the static attributes and the command pairs at all, and if it did, what
coraine answered.

`IoT-Agent` step 7 (README shows one Device, coraine returns two) is the T7 shape
again — the page shows one entry of a list.

## D15. ⭐ 81 rows of the tutorials' own seed data are refused - and § 5.2.2.5 says we may

Not visible in any step's output, and the most consequential thing in the sweep
after C1. `Context-Providers` logs **nine** `400 Forbidden Characters` during
SEEDING, before the README's first step runs:

```
value of 'description' contains forbidden characters (< > " ' = ; ( ) — § 4.6.4)
```

The offending values are ordinary English:

| file | rows rejected | example |
|---|---|---|
| `data/device.csv` | **80** | `Beany's Animal Collar` |
| `data/agri-pest.csv` | 1 | `The spinose ear tick, is a soft-bodied tick …` |

The apostrophe is the whole problem.

### What the spec actually says

The two clauses split names from values, and they are not the same rule.

⚠️ First, the citation itself is stale. `§ 4.6.2` / `§ 4.6.4` are **CIM 009
v1.9.1** numbers; in the current **TS 104-175** they are **§ 5.2.2.3 "Supported
names"** and **§ 5.2.2.5 "Supported content"**, word for word the same text. The
error message and `name_content_validation.test` both still say 4.6.x. Worth a
sweep of its own - `~/git/ngsild-specs/CIM009-v1.9.1_to_TS104_clause-map.tsv`
is the mapping.

**§ 5.2.2.3 "Supported names"** is the NAME rule, and it is a grammar, not a
character blacklist: `name = unicodeLetter *nameChar`, where `nameChar` is a
Unicode Letter, Number or `_`, with an optional `prefix:` form. Every one of
`< > " ' = ; ( )` is excluded by construction, along with everything else that
is not a letter, a number or an underscore. On a violating name, implementations
"should raise an error of type BadRequestData".

**§ 5.2.2.5 "Supported content"** is the VALUE rule, and it forbids nothing. It
lists those same eight characters and then says implementations "should decide
how to resolve the possible security problems that may be generated by the
data", adding that **"in all cases, implementations shall preserve the
representation of the content of the values … and return the original content
when replying to context consumption requests"**, and that "if implementations
decide to raise an error, the error shall be BadRequestData".

### So where does that leave coraine

**Conformant.** Raising `400 BadRequestData` is one of the resolutions § 5.2.2.5
explicitly allows, and that is exactly what coraine returns. The behaviour is
deliberate and covered by `name_content_validation.test`. It is a **D**, not a
bug, and nothing here is being changed on a tutorial's say-so.

**But it is the strictest end of a "should", and it has a cost this sweep
measured:** 81 rows of the FIWARE tutorials' own data, none of it hostile, none
of it exotic - an English possessive. Any broker those tutorials work on has
made the other choice. The clause's own "shall preserve … and return the
original content" reads as a nudge towards accepting and defending at the point
where a value could be read as structure, rather than at the door.

### The injection defences are already at the boundaries

Checked, because this is what the 400 is there to buy, and the answer decides
whether dropping it costs anything. Every place a value can become *query text*
escapes it at that point:

| boundary | what already happens |
|---|---|
| mongoc write | BSON through the driver - typed, never text |
| mongoc `$expr` (built as a JSON string) | `jsonStrEscape()` - `"`, `\`, `\n\r\t`, `\uXXXX` for controls |
| mongoc field names | `mongocEscapeDotsInKey()` - `.` → U+FF0E |
| timescale write | `PQexecParams` with `$1`…`$n` |
| timescale q → SQL | `escapeSqlLit()` - `'` → `''` |
| mongoc geo pipeline | a compile-time constant, nothing interpolated |

⭐ The division of labour is the same one the two clauses draw. **Names** are
safe to interpolate into query text because § 5.2.2.3's grammar has already
excluded every dangerous character - which is why `attrPath` can go into the
`$expr` JSON unescaped. **Values** are unconstrained by design, so they are
escaped where they would otherwise be read as syntax.

⚠️ And nothing is *stored* escaped, which is what keeps this simple: the escape
lives inside one statement and dies with it, so "was this already escaped?"
never has to be answered. Encoding at rest would ask exactly that question, and
would need an escape-the-escape rule to answer it. Cost today is zero on the
write path and confined to the query builders, which already pay it.

So the 400 is not load-bearing for injection safety. What is missing is the
*proof*: a functest that pushes `'; DROP …`, `<script>`, `"` and a backslash
through both DB plugins and asserts the value returns byte-identical - which
§ 5.2.2.5 requires regardless of which way the decision goes.

⚠️ **KZ's call, not a defect report.** Recorded here because the tutorials are
what made the cost concrete, and because it is a good Athens agenda item: a
"should" that splits implementations is exactly what the interop session is for.

## D16. Minor, seeding-time, unexplained

- One `kjParseValue: JSON Parse Error: invalid value` during seeding in
  `Entity-Relationships`, `Merge-Patch-Put` and `Context-Providers`, always at
  ~7.7 s. One malformed body from the tutorial app's own startup, not from any
  README step. Harmless; identify it when the stack is next up.
- `Context-Providers` logs one `registration overlaps with locally-stored entity
  'urn:ngsi-ld:Animal:cow001'`. Expected for an exclusive registration over an
  entity the broker already holds, but the tutorial does not mention it.

## T10. Five stacks hang provisioning devices, and it is NOT the broker

`Concise-Format`, `Short-Term-History`, `Time-Series-Data`,
`Verifiable-Credentials` and `Big-Data-Spark` all reach
`⏳ Link Devices to Animal entities` and stop there.

`link-devices` POSTs 100 device provisionings to the **IoT Agent** on `:4041`,
not to the broker. Two of the logs show `IoT Agent HTTP state: 000`, i.e. the
agent never answered. A side-service failure that blocks the tutorial, not a
coraine finding.

⚠️ My own rig broke this agent once already, in a way worth not repeating: I had
moved `MONGO_DB_PORT` to dodge a host-port collision, and that variable also
builds `IOTA_MONGO_URI`, so the agent could not reach mongo and died in a restart
loop. That is fixed — the host PUBLICATION is commented out instead — and the
current failure has some other cause.

## Driver corrections needed (mine, not the tutorials')

- **The container broker runs with no trace levels**, so `broker.log` carries
  only the ungated `W:`/`E:`/`X:` lines and is empty whenever nothing went wrong
  (7 of the 11). Useful as a negative signal - see T14 - but not enough to tell
  "never sent" from "sent and dropped". Phase 3 should add `-traceLevels` to the
  container's command, as the functests do.
- The comparator counts a body-less `204` as "no parseable response" and flags it
  (T8, step 11). It should read the status line first.
- ⛔ **`non-2xx: 0` means nothing.** It greps `responses.txt` for `HTTP/1.1 4xx`,
  and most README curls do not pass `-i`, so the status line is never captured.
  Context-Providers reported `non-2xx: 0` with nine 400s in the broker log. Force
  `-w '%{http_code}'` onto every extracted step.
- Two verdicts were wrong in both directions: four stacks reported as "failed to
  start" had actually reached the last seeding step and hit my timeout, and
  `Big-Data-Spark` reported as "broker not answering" had started fine — the
  health check fired too early.
- `IoT-Sensors` extracts **one** broker step of 22 fenced blocks. It is pure
  device simulation with no NGSI-LD requests, so there is nothing to test.
