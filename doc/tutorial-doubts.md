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

### ✅ DONE - the pristine second copy

⭐ **KZ's suggestion is the structural one**, and it is implemented: a second,
PRISTINE parse of the core context beside the rewritten one, built from the
context's own `body` so one mechanism covers both the embedded and the
downloaded init path. It deleted the `coreIriHT` table outright, turned
`corLdPrefixExpand`'s lookup into an O(1) probe, made the prefix array an
ordinary derived cache instead of a timing-sensitive pre-rewrite snapshot, and
**revived `corLdCompact` step 3**, which had never once fired.

⚠️ One trap, found by the suite rather than by reading: the pristine items are
handed OUT - `corLdExpand` returns one as `*itemPP` and `corLdExpandTree` then
ORs `itemP->flags` onto the node, which is how a structural member is told from
a sub-attribute. `coreContextClassifyFlags` ran on the working copy only, so the
pristine items ORed in ZERO and `observedAt` sent in its expanded spelling
stopped being structural and was normalized into a Property
(*"'observedAt' must be a string"*). Classifying both copies fixes it. Anything
handing out items from a second parse has to classify it.

The original reasoning follows.

 The rewritten copy stays exactly as it is
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

## C4. ✅/⚖️ RESOLVED, in two halves - one was a `shall not`, one is an ambiguity

Chasing this down changed what it is. Both halves are settled now, but not the
same way.

### The `json` half: a `shall not`, fixed

Clause 5 defines a JsonProperty's `json` as *"Raw unexpandable JSON which **shall
not be interpreted as JSON-LD** using the supplied @context"*. We interpreted it:

```
in : "json": { "https://uri.etsi.org/ngsi-ld/default-context/foo": 1 }
out: "json": { "foo": 1 }
```

`corLdCompactTree`'s `compactObject` recursed into every sub-object with an
`opaqueKeys` escape only for `@container` terms. It now also skips `KJF_VK_JSON`,
matching what `corLdExpandTree` has always done on the way in.

### The `value` half: an ambiguity the wire format cannot resolve

⚠️ **My first analysis was wrong, in both directions, and the suite caught it.**

I said the fix was to mirror the expansion side's full rule (`VK_VALUE |
VK_JSON | VK_VALUELIST`). Doing that broke `create_entity_simplified`: `num`
came back as `.../default-context/num`. The reason is that a Property's value
reaches expansion in **two shapes**, and only one is recognisable there:

| input shape | at expansion time | so the value's keys are |
|---|---|---|
| normalized - `"P": {"type":"Property","value":{…}}` | the `value` key is present and carries `KJF_VK_VALUE` | left alone |
| simplified - `"P": {…}` | the attribute IS the value; nothing marks it as one | `@vocab`-expanded like any other term |

Compaction has to undo the second case, so `value` cannot be opaque on the way
out. And that leaves a genuine ambiguity rather than a defect: **a key that is
literally a default-context IRI is indistinguishable, on the way out, from a
short key that was expanded on the way in.** It cannot be preserved without
knowing which it was, and nothing on the wire says.

So `property_value_opaque_json` step 12 now asserts the strip **deliberately**,
with the reasoning beside it, and step 13 asserts that `json` survives.

⭐ I had also written that GeoProperty was the blocker here - that its value keys
are stored expanded and need the walk. **That was wrong too:**
`ldGeoValueUnexpand` strips the geojson prefix before the DB model is built,
because a mongo 2dsphere index needs standard GeoJSON field names. Geo is stored
SHORT, so there was never anything in a geo value for this walk to compact.

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

⚠️ **Withdrawn (2026-09-19): the `x-ngsiv2-normalized` half.** I wrote that the
tutorial asks for an Orion-LD extension without saying so. It says so two lines
below the list: *"It is also possible to request that the Orion-LD context
broker pre-applies a compaction operation"*, and *"The set of available custom
formats will vary between Context Brokers."* Correctly flagged all along; my
reading was careless.

(The list does misspell `x-nsgiv2` twice, which is in the PR.) § 5.2.12 allows `normalized`, `concise`,
`simplified` and `keyValues`; coraine lists exactly those in its 400.

⚠️ **Withdrawn too: the hardcoded subscription id.** That id sits in a *Delete a
Subscription* example introduced by "This example deletes the Subscription with
`id=…`" - an illustrative value a reader substitutes, which is ordinary
documentation practice. My extractor ran it verbatim and the 404 is mine, not
the tutorial's.

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

## T14. ✅ ANSWERED in phase 3 — `IoT-Agent` step 11, a command device with no attributes

`GET /entities/urn:ngsi-ld:Device:water001` returns `{"id":…,"type":"Water"}` and
nothing else; the README expects the static attributes and the command pairs
(`on_status`, `on_info`, …). `entity_type: "Water"` is correct — that is what the
provisioning asks for — so the type is not the problem; the missing attributes
are.

⭐ **Settled in phase 3, with traces on: the forward is made and the registered
provider supplies nothing.** See the phase-3 section at the end - coraine sends
`GET http://devices/…/water001` with the tenant and `Via` in place, and the
merged answer is still `{id, type}`. So this is a **T**, and the open question
is for the tutorial author: does the `devices` provider serve those attributes
at all?

The original note follows. The broker log narrowed it even while empty:
`IoT-Agent` logged **nothing at all** - zero `E:`/`X:` lines - so coraine
refused nothing.

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

## D15. ✅ DONE - the § 5.2.2.5 scan is gone; the restrictions are on NAMES

`Context-Providers` logged **nine** `400 Forbidden Characters` during SEEDING,
before the README's first step ran, on nothing worse than an apostrophe:

| file | rows rejected | example |
|---|---|---|
| `data/device.csv` | **80** | `Beany's Animal Collar` |
| `data/agri-pest.csv` | 1 | `The spinose ear tick, is a soft-bodied tick …` |

**KZ's ruling, 2026-09-18: the restrictions belong on NAMES.** And the two
clauses do split that way in effect, even though the eight characters are listed
in the values one:

- **§ 5.2.2.3 "Supported names"** is a grammar - `name = unicodeLetter
  *nameChar`, Letter / Number / `_`, optional `prefix:` - which excludes every
  one of those characters by construction, along with all other punctuation.
  Unchanged, and `ldIsValidName` still enforces it.
- **§ 5.2.2.5 "Supported content"** lists them as a hazard in VALUES and forbids
  nothing: implementations *"should decide how to resolve"* it, and *"in all
  cases ... shall preserve the representation of the content of the values ...
  and return the original content"*. Refusing was permitted; so is accepting.

⭐ **The scan turned out to be the only thing in its own walk.** Removing it
removed `ldCheckNamesAndContent`, `checkEntity`, `checkAttribute`,
`checkValueContent` and `isStructuralKey` entirely - the file's own comments
already said names were validated elsewhere, during expansion - plus
`ldStringHasForbiddenChars`, which had **zero callers**.

**Injection safety never depended on it**, and that was checked rather than
assumed: every place a value can become query TEXT escapes it at that boundary -
`jsonStrEscape` for the mongoc `$expr` JSON, `mongocEscapeDotsInKey` for field
names, `PQexecParams` for timescale writes, `escapeSqlLit` for the TRoE q-to-SQL,
and the geo pipeline is a compile-time constant. Nothing is stored escaped, so
the "was it already escaped?" question never arises.

`name_content_validation` steps 07-13 now assert the opposite of what they used
to: all eight characters in a string, in an array element and in a
sub-attribute, byte-identical out; `Beany's Animal Collar`; and an
injection-shaped value that round-trips **and** is matchable by `q` - which is
what actually demonstrates the escaping works.

## D16. Minor, seeding-time, unexplained

- One `kjParseValue: JSON Parse Error: invalid value` during seeding in
  `Entity-Relationships`, `Merge-Patch-Put` and `Context-Providers`, always at
  ~7.7 s. One malformed body from the tutorial app's own startup, not from any
  README step. Harmless; identify it when the stack is next up.
- `Context-Providers` logs one `registration overlaps with locally-stored entity
  'urn:ngsi-ld:Animal:cow001'`. Expected for an exclusive registration over an
  entity the broker already holds, but the tutorial does not mention it.

## T10. ⚠️ SUPERSEDED by T15 — my diagnosis was wrong

Phase 1 concluded "the IoT Agent never answered". The agent answers fine; the
wait loop gives up too early, and `link-devices` dies on an unretried empty
reply under `set -e` (T15, with the evidence). Three of the five now run.
Original text below.

### The original, wrong, note


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

# Phase 3 — 2026-09-18, after the fixes

Three tutorials re-run on an image built from `main` (`907980e`), not eleven:
only the findings with a coraine-side change under them are worth re-running,
and the rest of phase 1's are tutorial errors no fix moves. Scope and the
`--traceLevels` rig change are in `.runner/RIG-MODIFICATIONS.md` § 4.

⭐ The image was smoke-tested for the three fixes before spending an hour on the
stacks — an apostrophe value accepted, `ngsi-ld:speed` expanding to the right
IRI, `Porpetry` refused. `make docker` stages COMMITTED state, so a stale lib
clone would otherwise have produced an hour of meaningless results.

## ✅ D15 confirmed on real data

`Context-Providers` broker errors: **12 → 3**. All nine
`400 Forbidden Characters` are gone — the seeding failures that phase 1 found
before the README's first step. That is 81 rows of the tutorials' own data that
the broker was refusing on an apostrophe, now accepted.

The remaining three are D16's, untouched and unexplained: one `kjParseValue`
parse error, one `invalid value`, and the `registration overlaps with
locally-stored entity 'urn:ngsi-ld:Animal:cow001'`.

⚠️ Not claimed: that every row landed. Steps 1 and 3 still answer `[]` for
`type=Animal`, but they legitimately do — they query before anything is
registered, the README shows `[]` too, and the comparator does not flag them.
What is demonstrated is that the refusals stopped.

## ✅ T14 answered — coraine forwards; the provider supplies nothing

Phase 1 left this open because `broker.log` was empty: a Device coming back as
`{"id":…,"type":"Water"}` with no error anywhere to explain the missing static
attributes and command pairs. With traces on, the log is 43 KB and says it
plainly:

```
ldDistOp.c[591]: forward request: GET http://devices/ngsi-ld/v1/entities/urn:ngsi-ld:Device:water001
ldDistOp.c[599]: forward request param: sysAttrs=true
ldDistOp.c[606]: forward request header: Via: 1.1 172.18.1.8:1026:openiot
ldDistOp.c[606]: forward request header: NGSILD-Tenant: openiot
```

So the registration matched, the forward was made, correctly addressed, with the
tenant and the loop-avoidance `Via` in place — and the merged answer is still
`{id, type}`. The commands are registered separately and forwarded too
(`PATCH http://iot-agent:4041/…/water001/attrs/on`).

⇒ **Not a coraine bug.** The attributes live behind a registration pointing at
the tutorial's `devices` context provider, and that provider returns nothing for
the entity. Zero broker errors, so nothing was refused or failed to parse.

⚠️ What is still unknown, and needs the provider's own log rather than the
broker's: whether `devices` 404s, answers an empty entity, or was never meant to
serve those attributes and the agent registered them to the wrong place. That is
a question for the tutorial author, and T14 becomes a **T**, not a **C**.

## Unchanged, as expected

Flags stayed at 6 / 3 / 2 for `Context-Providers`, `IoT-Agent` and
`IoT-Agent-JSON`. Every one of them is a tutorial error — `-X` eating the URL,
an abridged expected block, the agent's own `attributes` vs `polling` drift — so
no coraine fix could have moved them, and none did. That they did not move is
itself worth recording: it says the fixes were scoped to what they claimed.

# Phase 6 — the three tutorials that had NEVER run

`Concise-Format`, `Short-Term-History` and `Time-Series-Data` reached their
first steps for the first time. Everything below is new ground: phase 1 never
got past their seeding.

## ⭐ T15. Why they never ran — `link-devices` under `set -e`, with no retry

Two separate faults, and the second is the one that actually blocked them.

**The agent wait exits too early.** Four tutorials use

```bash
while [ "$( … -w %{http_code} 'http://iot-agent:4041/version')" -eq 000 ]
```

which stops waiting the moment the port answers **anything** non-`000` —
including the `503` a Node.js agent serves while still initialising.
`IoT-Agent`, `IoT-Agent-JSON` and `Big-Data-Spark` instead wait on the container
health status, and do not have the problem. The correlation is exact across all
eight tutorials checked.

**And `link-devices` has no retry.** It POSTs several ~30 KB device batches, and
the agent **intermittently** closes the connection without replying — curl exit
**52**, "empty reply from server", most often on the batch immediately following
`provision-devices`. The script runs under `set -e`, so one empty reply kills it
silently at the banner.

⭐ Proven intermittent rather than inferred: the identical payload against the
same agent gave **exit=52, then exit=0**, back to back. A single device is
accepted with `201` throughout, and 31 KB is nowhere near the agent's 1 MB
`expressLimit`, so it is not a size limit.

**Fix for the author:** wait on health, and retry the batch (or drop `set -e`
around it). With both worked around in the rig, all three tutorials run.

## T16. `Concise-Format` — a doubled slash from a missing URL variable

Step 14 requests `http://localhost:1026//ngsi-ld/v1/entities/`. coraine answers
`400 Invalid URL Path: empty path segment in '//ngsi-ld/v1/entities' (likely a
missing URL variable)`, which is exactly right and names the cause.

## T17. `Concise-Format` — a malformed JSON payload

Step 18, `POST /entityOperations/update?options=replace`: `400 request body is
not valid JSON`. The parser points at it — *"expecting comma or end of object,
Pos 349: `"observedAt": "2022-03-01T15:00:00.000Z"`"* — a missing comma in the
README's own payload.

## T18. ✅ RESOLVED — `Concise-Format` step 15 tries to re-type an Attribute by PATCH

An earlier step creates `category` in simplified form (`"category": "sensor"`),
so it is a **Property**. Step 15 then does

```
PATCH /entities/urn:ngsi-ld:TemperatureSensor:001/attrs/category
{ "vocab": [ "sensor" ] }
```

i.e. concise form whose value key implies **VocabProperty**. coraine refuses:
*"Attribute '…/category' of type Property carries its value in 'value', not
'vocab'"*.

**KZ, 2026-09-18: an Attribute's type changes only on a full replace** — PUT
`/entities/{id}/attrs/{attrId}`, or a POST replace (`options=replace`). Every
partial path leaves it alone. So coraine is right and the step is a **tutorial
error**: demonstrating VocabProperty is the point of the page, but a PATCH is
not the way to do it.

⚠️ **The spec does not say so in general**, which is worth separating from the
verdict. Checked both documents: exactly one statement constrains re-typing, in
TS 104-176 clause 7's `format` row for **Merge Entity with
`format=simplified`** — *"the `type` field of the Attribute shall remain
unchanged (any attempt to modify the `type` of an Attribute shall result in a
`BadRequest` error)"*. § 10.2.5 Partial Attribute update does not mention `type`
at all, and neither does Update Attributes.

So our rule generalises the one explicit case rather than following stated text,
and an implementer who read the silence as permission would behave differently.
Raised as **spec-doubt #128** (`ngsild-specs/ts-104-175/spec-doubts-2.md`), with
the fix wanted being to state it once in § 10.2.5 rather than inside a
parameter's remarks.

⚠️ coraine's concise inference is NOT at fault — verified directly, all six value
keys infer their type correctly on a create (`value`→Property,
`object`→Relationship, `vocab`→VocabProperty, `languageMap`, `json`,
`valueList`), including under a user `@context` and via batch. The refusal is
specifically about re-typing something that already exists.

## T19. ⚠️ WITHDRAWN — `<current_time>` is a documented placeholder

I recorded *"the README carries the literal placeholder `<current_time>`, so the
step cannot be run as printed"*. It is documented as a placeholder two lines
later: *"`timerel=before` and `timeAt=<current_time>` are required parameters.
`<current_time>` is a date-time expressed in UTC…"*. A reader substitutes it.
The 400 is my driver executing documentation verbatim.

## ⚠️ Not a finding: `pageSize` — caused by MY redirect

Six `400 Unknown/unsupported URL parameter: pageSize`. That is rig modification
§ 3 meeting Mintaka's own API: `pageSize` is Mintaka's paging parameter, NGSI-LD
uses `limit`, and I redirect those queries to coraine. **Not reportable as a
tutorial defect.**

It is still worth telling the author one thing: those temporal steps are written
against Mintaka's API, not the NGSI-LD temporal API, so they are not portable to
a broker that implements temporal itself.

## ⭐⭐ T21. `Big-Data-Spark` puts its Docker network on PUBLIC address space

The reason it never ran, and the best find of the sweep.

```
Big-Data-Spark/.env:  SUBNET=182.18.1.0/24
```

`182.18.1.0/24` is **public** address space, allocated to APNIC. Every other
tutorial hardcodes `172.18.1.0/24` in `common.yml`; `Big-Data-Spark` is the only
one that parameterises the subnet, and the value it parameterises it with is not
private.

**The symptom is confusing, which is why it took three attempts.** The stack
starts, `./services coraine` exits 0, its own output says *"coraine is now
running and exposed on localhost:1026"*, the container reports **healthy**, its
log is empty, its arguments are right, and `docker ps` shows
`0.0.0.0:1026->1026/tcp`. And yet:

```
curl localhost:1026/admin/health   ->  exit 28 (timeout), not connection refused
container IP                       ->  182.18.1.8
curl 182.18.1.8:1026/admin/health  ->  timeout
```

A bound port that accepts and never answers, because the container's address is
a real internet host as far as the kernel's routing is concerned.

⚠️ It also quietly hijacks 256 public addresses on any machine that runs it.

**Fix for the author:** `SUBNET=172.18.1.0/24`, matching every other tutorial.
With that one change the tutorial runs: 4 steps, no broker errors.

⛔ I recorded this twice as "BROKER NOT ANSWERING after start" and once
"blamed" my own health check for it - which was also true (that check WAS a
single curl, fixed) but was not this. Two faults with one symptom, and fixing
the visible one hid the real one for two more runs.

## T20. `Short-Term-History` step 1 — the expected block is a different entity

The step queries `temporal/entities/urn:ngsi-ld:Animal:cow002?lastN=3` and gets
an Animal - `birthdate`, `fedWith`, `legalId`, `species`, `weight`. The README's
expected output is a **Device**: `category`, `controlledAsset`,
`controlledProperty`, `supportedProtocol`. Two different entities; the T7 family.

⭐ Worth noting what this step proves rather than only what it flags: coraine's
OWN temporal API answered `lastN=3` correctly, with per-attribute instance
arrays and `instanceId`s. That is the path the Mintaka redirect (rig § 3) exists
to exercise, and it works.

## T22. Subscription listings omit three members the spec requires

`Time-Series-Data` step 3 and `Big-Data-Spark` step 2 both expect

```
[description, entities, id, notification, throttling, type, watchedAttributes]
```

and get those plus **`jsonldContext`, `notificationTrigger`, `status`**. All
three are legitimate: `notificationTrigger` and `status` are Subscription
members in clause 5, and `status` is explicitly *"Read-only. Provided by the
system when querying the details of a subscription"* - so returning it is not
optional. `jsonldContext` is a core-context term.

The READMEs predate them. T7 family again, and the same advice: the expected
blocks want regenerating against a current broker.

⇒ **15 of 16 tutorials have now been run.** Only `Verifiable-Credentials`
remains, deliberately - it pulls in the Data Space Connector, which is an
identity stack rather than a broker test.

# Upstream PRs — what was actually proposed, 2026-09-19

Eight PRs against `FIWARE/tutorials.*`, base `NGSI-LD`, each from a FRESH clone
so no rig modification could leak in (every diff checked for `RIG` / `coraine` /
`traceLevels`):

| repo | PR | fix |
|---|---|---|
| Big-Data-Spark | #12 | `SUBNET` off public address space (T21) |
| Big-Data-Flink | #16 | blank line breaking a curl continuation (T12) |
| Concise-Format | #2 | doubled slash, missing comma, agent readiness (T16, T17, T15) |
| Context-Providers | #23 | `-X <url>` eats the URL, ×4 (T8) |
| Extended-Properties | #1 | stale data-model IRIs, ×2 (T11) |
| Subscriptions | #19 | `%22` in a JSON body ×4, `x-nsgiv2` typo ×2 (T9) |
| Short-Term-History | #17 | agent readiness (T15) |
| Time-Series-Data | #57 | agent readiness (T15) |

⭐ **The retry fix was wrong on the first attempt.** `--retry 5 --retry-delay 5`
still failed in a real `services` run - about 25 s of retrying is not enough
while the agent is saturated straight after `provision-devices`.
`--retry 10 --retry-delay 10` was then tested end to end (`services` exits 0,
the tutorial runs) before being proposed. Worth the extra hour: the alternative
was someone else finding it in review.

## Findings deliberately NOT sent upstream

- **T7, T13, T20, T22** — expected blocks that have drifted from what a current
  broker returns: an abridged `pick` result, `attributes` vs `polling` from a
  newer IoT Agent, a Device shown where an Animal is queried, subscriptions
  missing `jsonldContext` / `notificationTrigger` / `status`. All real, but the
  fix is regenerating example output against a current stack, which is the
  author's call and a much larger diff than a defect fix.
- **T14** — answered (the forward is made; the registered provider returns
  nothing) but the remaining question is about the tutorial's own `devices`
  provider, which wants asking rather than patching.
- **T18** — coraine refuses to re-type an Attribute by PATCH and the tutorial
  expects it to work. Our reading is defensible and the spec does not state the
  general rule, so this went to ETSI as spec-doubt #128 instead of to the
  author.

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
