# NGSI-LD v1.9.1 — what's still missing (audit 2026-05-01)

After the snapshots + phase #146d push, the broker is **~98% spec-conformant**.
This file is the working backlog. Cross-references the prior audit at
`~/git/fwLibs/claude/spec/gap-report.md` (which lists items already closed
+ phantoms — not duplicated here).

Methodology: 5 parallel agents, one per spec slice (§ 5.6/5.7, § 5.8, § 5.9-5.14,
temporal+§ 5.16+§ 5.15, ch4/ch6/ch7), grepping `swBroker/src/` and
`swNgsild/` against the spec markdown under `~/git/fwLibs/claude/spec/`.


## Tier A — small, real, worth doing

| # | § | Gap | Status |
|---|---|-----|--------|
| 1 | 5.7.2 / 6.4.3.2 | `?containedBy=` URL param silently ignored (join cycle prevention, 0..1 optional) | ✅ done 2026-05-01 (fwNgsild f172679, fwBroker eb18127); sw test `url_param_contained_by`, fw test `url-param-contained-by` |
| 2 | 5.7.2 / 6.23.3.1 | `?expandValues=` URL param not implemented (JSON-LD type-coercion expansion, MAY-not-MUST) | ✅ already implemented (sw+fw `ldQParse.c:531-548`); audit was wrong. fw functest mirror added 2026-05-01 (fwBroker 20b1b1d) |
| 3 | 5.2.12 | `ngsildConformance` member on Subscription accepted but not applied — TODO already at `ldDistOp.c:220` (back-compat path for older brokers) | ✅ done 2026-05-02 (swNgsild + fwNgsild). New `ldConformanceDowngrade` module implements § 4.3.6.8 Tables 4.3.6.8-1/2/3 transformations: < 1.9 strips entity+attr expiresAt and attr valueType; < 1.8 reformats Json/Vocab/List/ListRelationship to Property/Relationship; < 1.4 strips scope and reformats LanguageProperty; < 1.3 strips datasetId/observedAt/unitCode and collapses multi-instance arrays + multi-type arrays. Validated as `M.m`; applied to subscription notification body before render. Distop forward back-compat (`ldDistOp.c:220` TODO) still pending — separate scope. Functests `subscription_ngsild_conformance` / `subscription-ngsild-conformance` |
| 4 | 5.10.2 | Discovery `GET /csourceRegistrations` returns full RegistrationInfo even when only a subset matches the filter — § 5.10.2.5 wants the response filtered | ✅ done 2026-05-01 (fwNgsild d52bee7, fwBroker 0268e48); functest `csource_reg_discovery_filtered_info` / `csource-reg-discovery-filtered-info` |
| 5 | 5.10.2 / 6.3.10 | Discovery returns 200 even when results > limit — should emit Link rel=next/prev/first (audit's "206 + Content-Range" was overstated; per § 6.3.10 that's temporal-only). Also `NGSILD-Results-Count` on `?count=true`. | ✅ done 2026-05-01 (fwBroker 95faee7); functest `csource_reg_discovery_pagination` / `csource-reg-discovery-pagination` |
| 6 | 5.11.7 / 5.11.2 | `timeInterval` periodic CSR-subscription notifications | ✅ done 2026-05-01 (fwNgsild 9a483b4, fwBroker 5909169); refactored `ldPernotLoop` into a shared `ldPeriodicLoop` engine (1-Hz tick over registered consumers); pernot + CSR-Sub timeInterval both register with it. Functest `csr_subscription_time_interval` / `csr-subscription-time-interval` |
| 7 | 6.3.2 | `405 Method Not Allowed` returned without the required `Allow:` header | ✅ done 2026-05-01 (fwHttp 66236f0, fwBroker 0277ad2); functest `http_405_method_not_allowed` / `http-405-method-not-allowed` |
| 8 | 6.3.2 | `411 Length Required` and `413 Request Entity Too Large` — no handlers (transport-level limits not enforced) | ✅ done 2026-05-01; broker `--maxRequestSize` / `-mrs` MiB CLI (default 2 MiB, 0 = no cap; robotics use-case for large attribute payloads). 411 emitted body-less per § 6.3.4; 413 streaming + Content-Length pre-check. Functest `http_411_413_body_limits` / `http-411-413-body-limits` |


## Tier B — MQTT binding (ch7) tail

| # | Gap | Status |
|---|-----|--------|
| 9 | `notifierInfo.MQTT-Version` ignored — hardcoded; spec default mqtt5.0 | ✅ done 2026-05-02 (fwNgsild + swNgsild). sw stack honors mqtt5.0 / mqtt3.1.1 via libmosquitto's `MOSQ_OPT_PROTOCOL_VERSION`; both stacks validate notifierInfo (rejects bad QoS / Version values). fw's `fwMqtt` library is still 3.1.1-only — accepted at API surface, downgraded by publisher; documented in `fwMqtt.h`. Functests `subscription_notify_mqtt_validation` / `subscription-notify-mqtt-validation` |
| 10 | ~~`notifierInfo.MQTT-Retain` ignored — hardcoded `false`~~ | ❌ retracted 2026-05-02 — phantom; not in v1.9.1 (Table 7.2-1 lists only QoS and Version) |
| 11 | MQTT QoS=1 / QoS=2 + MQTT-Version round-trip functest | ✅ done 2026-05-02 (sw only — fwMqtt is QoS-0-only). Functest `subscription_notify_mqtt_qos_version` exercises QoS=1+mqtt3.1.1 and QoS=2+mqtt5.0 |


## Tier C — distributed ops loose ends

| # | § | Gap | Status |
|---|---|-----|--------|
| 12 | 5.14.4.4 | `linkedMaps` in distributed EntityMap creation — was emitted empty | ✅ done 2026-05-02 (swNgsild + fwNgsild + getEntities both stacks). Forward branch: when `entityMapCreate=true`, switches from `GET /entities` to `GET /entityMaps` (the spec route for Create-EntityMap-Query-Entity); CP returns its own EntityMap with `id` + `entityMap` keys; local broker stores `(csr.regId → remote-EntityMap-id)` in `linkedMaps`. Functests `entitymap_linked_maps` / `entitymap-linked-maps`. Pagination optimization (using stored remote map id during page-fetch) deferred — current per-entity GET still works. |
| 13 | 5.7.4 | Multi-source temporal pagination (Content-Range across CSRs) — deferred per `project_temporal_distops_deferred` memory | deferred (by design until temporal distops land) |
| 14 | 5.7.4 | `orderBy` rejected across distops | ❌ retracted 2026-05-02 — by design (k-way merge breaks under offset/limit + split-entities; EntityMap / Snapshots are the answer for ordered distributed queries) |
| 15 | 6.3.18 | `local=true` parameter coverage on `/types/`, `/attributes/`, `/temporal/entities/` | ✅ done 2026-05-02 (swNgsild + fwNgsild). `LD_PARAM_LOCAL` added to `LD_PARAMS_GET_TEMPORAL_ENTITY` and `LD_PARAMS_DELETE_TEMPORAL_ATTR` (was 400 unknown-param). Functest `local_param_distops_suppress` / `local-param-distops-suppress`. Note: csourceSubscriptions excluded — Registry API is always local per § 6.3.18 |


## Tier D — minor / response shape

| # | Gap |
|---|-----|
| 16 | `endpoint.accept = application/geo+json` on a notification — broker doesn't convert to FeatureCollection (delivers regular JSON-LD) | ✅ done 2026-05-02 (swNgsild + fwNgsild). `endpoint.accept` parsed into LdSubCacheItem.endpointAccept; ldSubscriptionNotify converts the `data` field via ldToGeoJson when geo+json, and emits Content-Type: application/{geo+json,ld+json,json} on both HTTP and MQTT paths. Functests `subscription_notify_geojson` / `subscription-notify-geojson` |
| 17 | `NotificationParams.attributes` (deprecated alias for `pick`) — accepts but doesn't reject `id`/`type`/`scope` per spec | ✅ done 2026-05-02 (swNgsild + fwNgsild). `ldCheckSubscription.c` now rejects literal "id"/"type"/"scope" in `notification.attributes` array per § 5.2.12. Functests `subscription_attributes_alias_validation` / `subscription-attributes-alias-validation` |
| 18 | `NGSILD-EntityMap` header — request-side + distop-side | ✅ done 2026-05-02 (swNgsild + fwNgsild). Request-side: GET /entities accepts `NGSILD-EntityMap: <map-uri>` as alternative to `?entityMap=<id>` (last URL segment is the map id). Distop-side: when entity-map pagination forwards a per-entity GET to a CP, `linkedMaps[csr.regId]` is looked up and `NGSILD-EntityMap: /ngsi-ld/v1/entityMaps/<remote-id>` is included so the CP serves from its frozen snapshot. Implemented via new `ldDistOpSendReceiveEx` (extra-headers variant). Functests `entitymap_header_distop` / `entitymap-header-distop` |


## Tier E — phantoms (already retracted in prior audit, listed to avoid re-flagging)

- `notification.showCount` / `showStatus` / `lang` / `genericTriggers` / `endpointType` — not in v1.9.1
- Subscription state `erroneous` — not in v1.9.1 (only active/paused/expired)
- CSR fields `jurisdiction*`, `lastForwardingTimestamp`, `locationLifecycle` — not in v1.9.1
- `?observedAt=` / `?lang=` on Update/Append Attributes — phantom (only on Merge per § 6.5.3.4)
- Batch CSR ops — not in spec
- Distops on CSR CRUD itself — CSRs are local-only by design
- `Idempotency-Key` request header — non-mandatory
- `snapshotPriority`-driven eviction — field stored, no eviction policy mandated by spec


## Tier F — done since last audit (FYI, not actionable)

- Full § 5.16 Snapshots subsystem (CRUD, capture, distop capture, split-mode merge, persistence + reload, async background, lifetime parsing, write-guard 422, snapshot-aware temporal reads, clone temporal copy, timescale tenantDrop) — phases #142–#147
- All Tier-2 items from prior 2026-04-29 audit (q-weighted Accept, type-immutability on partial update, name+content validation, `LdContextNotAvailable`→504, etc.)


## Working order

Process Tier A in numbered order (1 → 8). Each item: sw first, fw mirror,
functest both stacks, commit + push fw. After Tier A, reassess priorities
for B / C / D against real-world demand.
