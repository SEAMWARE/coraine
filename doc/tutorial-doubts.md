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
