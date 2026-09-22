# FIWARE NGSI-LD Tutorials — status report

**For:** FIWARE TSC
**Period:** 2026-09-17 → 2026-09-19
**Scope:** the 16 tutorials on the `NGSI-LD` branch of `FIWARE/tutorials.*`

---

## 1. In one page

| | |
|---|---|
| Tutorials on the `NGSI-LD` branch | 16 |
| Run end to end against a broker | **15** (`Verifiable-Credentials` excluded on purpose — it brings up the Data Space Connector, an identity stack rather than a broker test) |
| Tutorials that could not complete before this work | **4** |
| Changes proposed upstream | **22 pull requests across 13 repositories** |
| Defects that were the broker's, not the tutorials' | **7**, all fixed |
| Questions referred to ETSI CIM | **2** |

Two defects stopped whole tutorials from running and had done so for some
time. Neither was in a broker.

---

## 2. How this was done

Each tutorial's README is a sequence of `curl` requests with the expected
response printed under it. That is a test suite that nobody was running. The
method was simply to run it:

1. Extract every request block from the README, in document order.
2. Bring the tutorial's own stack up with its own `services` script.
3. Execute the requests against it and capture every response.
4. Compare each response with the block the README prints beneath it.
5. For every difference, decide **which side is wrong** — the page, or the
   broker — by reading TS 104-175.

Step 5 is the whole job. A difference is not a defect until somebody has said
which side of it is correct, and roughly a third of the differences found so
far turned out to be the broker being right and the page being out of date.

⚠️ **The comparison has to look at values.** The first pass compared only the
number of entities returned and their top-level attribute names. That version
reported almost everything as passing. A `value` of `25` coming back as `999`,
or a *Property* returned as a *Relationship*, both scored "ok". Re-running with
a comparison that descends into the JSON changed the picture completely: **29
of 76 comparable steps differ**, against almost none before.

---

## 3. What was wrong, by class

### 3.1 Requests that do not do what the page says

Four distinct `curl` invocation bugs, all silent — the request succeeds, it
just is not the request the prose describes.

- **`curl -G -X <url>`** — the URL is consumed as the HTTP method, so the
  request goes to the wrong place. `Context-Providers`, ×4.
- **`curl -L -X GET … -d 'format=concise'`** — without `-G`, curl puts `-d`
  data in the request **body**. The broker never sees the parameter and returns
  normalized while the page shows concise. `Merge-Patch-Put`.
- **A blank line inside a line continuation** — the continuation ends there and
  the next line runs as its own command (`-H: command not found`), so the
  request goes out without its tenant header and queries the wrong tenant.
  `Big-Data-Flink`.
- **Percent-encoded quotes inside a JSON body** — `%22` is a URL escape and
  means nothing in a body; the `q` is rejected. `Subscriptions`, ×4.

### 3.2 Expected output that contradicts the page's own input

These are checkable without a broker at all — the response block disagrees with
a request printed earlier on the same page.

- A **VocabProperty shown as a bare string** in simplified format. The entities
  are created with `{"type":"VocabProperty","vocab":"sensor"}`, and TS 104-175
  § 5.3.2.4 is explicit that the simplified form keeps the wrapper
  (`"gender": {"vocab": "Male"}`). `CRUD-Operations`, `Entity-Relationships`.
- **`"batteryLevel": 0.8`** where the page's own creation payload sets `0.9`
  and nothing changes it in between. `CRUD-Operations`.
- A page that **contradicts itself**: step 1 creates `category` as a bare
  string — which is a *Property* — and step 7 expects a *VocabProperty* back.
  `Concise-Format`.
- **Stale data-model IRIs** that are not the ones the tutorial ships.
  `Extended-Properties`.

### 3.3 Infrastructure that fails intermittently

- **A race against the IoT Agent.** `addIoTDatabaseIndex` drops every
  collection in `iotagentjson` and then re-creates `devices`. The IoT Agent is
  already running and connected, and can create it first. On **mongo 6.0** —
  the version every tutorial pins — `createCollection` on an existing
  collection throws, `mongosh` exits 1, and the script runs under `set -e`, so
  the whole `services` invocation dies after the stack is up. **Nine tutorials
  carry this function verbatim.** Caught in `Big-Data-Flink`'s own CI, where
  the same commit then passed on re-run.

  ⚠️ Worth knowing: mongo 8 made `createCollection` idempotent, so **raising
  the pinned mongo version would hide this rather than fix it.**

### 3.4 Documentation that has drifted

Expected blocks that no longer match what a current broker returns: an
abridged `pick` result, `attributes` vs `polling` from a newer IoT Agent, a
Device shown where an Animal was queried, and subscription listings that omit
`jsonldContext`, `notificationTrigger` and `status` — the last of which the
spec marks read-only and *provided by the system*, so a broker has to return
it.

**These were deliberately not sent as pull requests.** Fixing them means
regenerating example output against a current stack, which is a much larger
diff than a defect fix and is a decision for whoever owns the tutorials.

---

## 4. The two that stopped tutorials running

### 4.1 `Big-Data-Spark` was on public address space

`.env` set `SUBNET=182.18.1.0/24`. That range belongs to APNIC; every other
tutorial uses `172.18.1.0/24`. The failure mode is unhelpful: the stack starts,
`services` exits 0, containers report healthy, the port shows as published —
and requests to `localhost:1026` **time out** rather than being refused,
because as far as the kernel is concerned the container's address is a real
host on the internet. It also occupies 256 public addresses for as long as it
runs.

One line. With it changed, the tutorial works.

### 4.2 Four tutorials died at "Link Devices to Animal entities"

Two independent causes, which is why it had survived:

- `waitForIoTAgent` loops *while the HTTP code is `000`*, so it stops waiting
  the moment the port answers anything at all — including the `503` the agent
  serves while still starting. Three tutorials already wait on container
  health; the others did not.
- `link-devices` posts several ~30 KB batches with no retry, under `set -e`.
  The agent intermittently closes the connection without replying (curl exit
  52), most often on the batch straight after `provision-devices`. One such
  reply kills the script with nothing printed.

Confirmed intermittent rather than a size limit: the same payload against the
same agent gave exit 52, then exit 0, back to back.

---

## 5. What was proposed upstream

22 pull requests, base `NGSI-LD`, each made from a fresh clone so that no
local test-rig modification could leak in.

| Repository | PR | Change |
|---|---|---|
| Big-Data-Flink | #16 | blank line breaking a curl continuation |
| Big-Data-Flink | #17 | IoT-Agent / mongo collection race |
| Big-Data-Spark | #12 | `SUBNET` moved off public address space |
| Big-Data-Spark | #13 | IoT-Agent / mongo collection race |
| CRUD-Operations | #24 | query by `id` alone is not a valid query |
| CRUD-Operations | #25 | two response blocks contradict their own input |
| Concise-Format | #2 | doubled slash, missing comma, agent readiness |
| Concise-Format | #3 | query by `id` alone, ×2 |
| Concise-Format | #4 | IoT-Agent / mongo collection race |
| Context-Providers | #23 | `curl -G -X <url>` eats the URL, ×4 |
| Entity-Relationships | #32 | VocabProperty shown as a bare string, ×3 |
| Extended-Properties | #1 | stale data-model IRIs, ×2 |
| IoT-Agent | #36 | IoT-Agent / mongo collection race |
| IoT-Agent-JSON | #12 | IoT-Agent / mongo collection race |
| IoT-Sensors | #20 | IoT-Agent / mongo collection race |
| Merge-Patch-Put | #2 | `format=concise` sent in the request body |
| Short-Term-History | #17 | agent readiness + device-batch retry |
| Short-Term-History | #18 | IoT-Agent / mongo collection race |
| Subscriptions | #19 | `%22` in a `q` inside a JSON body, ×4 |
| Subscriptions | #20 | IoT-Agent / mongo collection race |
| Time-Series-Data | #57 | agent readiness + device-batch retry |
| Time-Series-Data | #58 | IoT-Agent / mongo collection race |

---

## 6. What the tutorials found in a broker

The point worth making to the TSC: **the tutorials are a test surface nothing
else covers.** They exercise the API the way a new user does — through
documentation, across all four representation formats — and they found defects
that a conformance suite and a functional suite both missed.

Six broker defects found this way, all fixed:

- A JSON object inside a Property's `value` was rewritten on retrieval.
- An already-expanded core term was not recognised as a core term.
- An invalid attribute type was accepted, with a second `type` bolted on beside
  it, producing two members with the same name in one JSON object.
- Integers longer than 15 digits were truncated by the JSON renderer.
- The core context had been rewritten in place, with four downstream victims.
- Values were rejected for containing characters that § 5.2.2.5 does not in
  fact forbid — which is what stopped 81 rows of the tutorials' own seed data
  from loading.

And one worth reporting against ourselves: a query-selector rule was
**relaxed** on the strength of one tutorial page, and that cost two ETSI
conformance test purposes before the next nightly caught it. Reverted the same
day. The lesson generalises: a tutorial page is evidence that something is
confusing, not evidence that the specification says something else.

---

## 7. Referred to ETSI CIM

Two questions the tutorials surfaced that the specification does not settle.

- **May a partial update change an Attribute's `type`?** The rule is stated
  only for merge with `format=simplified`; every other partial path is silent.
  A tutorial step relies on the permissive reading.
- **What is the concise form of a Property whose value is a JSON object?**
  § 5.3.2.3 answers three ways: a NOTE says it equals the simplified form, the
  compaction algorithm says the `value` member is kept, and the expansion
  algorithm makes the NOTE's form an invalid payload. Implementations have had
  no feedback here because **the conformance suite does not exercise the
  concise format at all.**

---

## 8. Decisions for the TSC

1. **Pinned mongo version.** Every tutorial pins `MONGO_DB_VERSION=6.0`.
   Raising it would mask the collection race rather than fix it, so the PRs in
   § 5 should land first; after that the pin is worth revisiting on its own
   merits.
2. **`propertyNames` → `attributeNames` in registrations.** The spec marks
   `propertyNames` **Deprecated** and defines `attributeNames` as the synonym
   for it plus `relationshipNames`. One tutorial (`Context-Providers`) uses the
   deprecated spelling. **This change is blocked:** not every broker the
   tutorials are tested against parses `attributeNames` in a registration, so
   moving the page would break those runs. It needs the brokers first.
3. **Regenerating drifted expected output** (§ 3.4) — a decision, not a defect
   fix.
4. **Running the tutorials as CI.** See below.

---

## 9. Proposal: run the tutorials as a nightly check

The strongest argument is that this is **what a new user meets first** —
before any conformance suite, before anyone's functional tests.

It is mechanically straightforward: extract, run, compare. Three things would
make it useless if done naively, and each has a fix:

| Problem | Fix |
|---|---|
| The tutorials are upstream and move; an edit turns the build red with no code change | Pin each tutorial to a commit SHA, bump deliberately |
| The expected blocks are illustrative, not normative — most current differences are the page | Baseline the accepted differences; fail only on **new** ones |
| Timestamps, generated ids, unordered results, data-state drift | Ignore volatile keys; the residue goes in the baseline |

Report-only to begin with, so the baseline can be established without a
permanently red build.

⚠️ **The automation is not the work — the triage is.** 29 differences are
outstanding, each needing a judgement against the specification. Until they are
classified there is no baseline, and without a baseline the check is noise.
**Triage first, automate second.**

---

## 10. Not covered

- `Verifiable-Credentials` — deliberately, as above.
- The `NGSI-v2` branch — out of scope.
- Triage of the remaining 18 value-level differences (11 of 29 done). No new
  broker suspects among them so far; most are data-state drift and the
  documentation drift already described in § 3.4.
