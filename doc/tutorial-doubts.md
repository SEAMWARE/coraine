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
