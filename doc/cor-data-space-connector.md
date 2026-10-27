# A Cor Data Space Connector - a proposal

An alternative provider side for the FIWARE Data Space Connector (FDSC), built on coraine and the cor
libraries: the same tokens, the same ODRL policies, the same NGSI-LD API, the same consumer side, in a
fraction of the processes and the memory. A proposal, not a plan: nothing below is scheduled.

## What the provider side is today

The FDSC's provider side, as its provider guide describes it
([roles/provider](https://github.com/FIWARE/data-space-connector/blob/main/doc/deployment-integration/roles/provider/README.md),
data-space-connector 10.11.0). The data services a provider offers - an NGSI-LD broker among them - are not
part of it: they are external to the connector, deployed and managed on their own, and the connector protects
them.

**Required:**

| role | today |
|---|---|
| the provider's own identity | the consumer role's components: Keycloak (issues credentials to the provider's own users), the DID Helper (publishes the provider's DID document; the Identity Hub in its place when the data space uses the Decentralized Claims Protocol), a managed PostgreSQL for the IAM components |
| the token | VCVerifier: a verifiable credential presented (OID4VP), checked against the trust framework, exchanged for a JWT |
| which credentials a service takes | VCVerifier's configuration API (until 10.11.0, the Credentials Config Service; now disabled by default) |
| which issuers are trusted | the local Trusted Issuers List (TIL), with the Trust Anchor's registry |
| who sent a request | APISIX, the Policy Enforcement Point: it validates VCVerifier's JWT (`openid-connect`); all external access to a protected service goes through it |
| may it | OPA, the Policy Decision Point, running the Rego odrl-pap writes from the ODRL policies, asked by APISIX's `opa` plugin |
| the policies | odrl-pap, the Policy Administration Point |

**Optional:**

| role | today |
|---|---|
| a Scorpio broker | enabled by default, as the backend of the TM Forum APIs; the chart's examples also expose it as the provider's NGSI-LD data service, which the guide advises against in production: there, a separate broker |
| contracts | the TM Forum APIs (enabled by default) and Contract Management (enabled by default): on a product order, it registers the buyer's DID in the local TIL and, with odrl-pap deployed, writes the contract's ODRL policies |
| the marketplace | the Business API Ecosystem (disabled by default; needs MongoDB), or a central marketplace run by another participant |
| the Dataspace Protocol | fdsc-edc (disabled by default, usually together with the Decentralized Claims Protocol): an Eclipse Dataspace Components (EDC) control plane with FIWARE's extensions, listed among the DSP-compliant connectors in IDSA's Data Space Connector Report No. 2 (June 2026) |

## What a Cor Data Space Connector would be

| role | a Cor DSC | state |
|---|---|---|
| the data service, outside the connector | coraine, or any NGSI-LD broker | built |
| who sent a request, may it | `--auth fdsc` in the broker ([Authorization](authorization.md)) - VCVerifier's tokens, the ODRL policies as odrl-pap holds them, decided as the connector decides them | built; 67 cases of `test/fdscDiff`, 0 differences against the connector's own chain |
| the policies | coraine's `/policy` API (`--policyApiPort`): odrl-pap's API, the policies in coraine's store | built |
| the same decision in APISIX, for any NGSI-LD broker | `ngsild-pep` ([corApisixNgsild](https://github.com/SEAMWARE/corApisixNgsild)): `fdsc.so`'s code in an APISIX plugin | built; the 64 cases of the coraine commit its CI pins (`7239cf37`), 0 differences |
| the token | a verifier: OID4VP, the credential formats and DIDs the connector accepts, the JWT | not started |
| which issuers are trusted | a TIL, with the EBSI TIR API the verifier reads | not started |
| which credentials a service takes | the verifier's configuration API | not started |
| the provider's own identity: Keycloak, the DID Helper or Identity Hub | as they are | kept |
| the Dataspace Protocol | fdsc-edc as it is, where it is used | kept |
| contracts, the marketplace, the consumer side | as they are | kept |

What is built covers every decision on a request to the data. What is not started is the authentication side:
the verifier, its credentials configuration and the TIL (in FIWARE's code: 0.9 MB of Go of its own plus the Trustbloc libraries, 0.2 MB and 0.1 MB
of Java). The Dataspace Protocol stays with fdsc-edc, which already passes the DSP test kit: a Cor DSC
changes what runs beneath it, not the protocol a data space speaks.

## What it would gain

Measured 2026-10-14 on one machine ([Performance](performance.md#the-decision-in-the-broker-or-in-a-gateway)
has the method, every scenario and every run):

**Throughput** - the same decision, 20 policies, one core for the broker and one for each gateway process:

| | retrieve /s | PATCH /s | create /s |
|---|---:|---:|---:|
| coraine, no security | 78 190 | 108 573 | 58 716 |
| coraine, `--auth fdsc` | 72 615 | 98 928 | 53 374 |
| coraine behind APISIX + `ngsild-pep` | 34 174 | 34 185 | 28 546 |
| coraine behind APISIX + OPA (today's chain) | 2 890 | 2 069 | 2 780 |

**CPU a request** - today's chain spends 210 µs in APISIX and 320-500 µs in OPA on each request; the
decision in the broker costs about 0.8 µs.

**Footprint** - the provider side in containers as the chart's examples deploy it (data-space-connector
10.10.3, with Scorpio as the NGSI-LD data service), against one coraine (the broker, `--auth fdsc`,
`/policy`, durable store), with the same 10 000 entities:

| | today's provider side | coraine |
|---|---:|---:|
| processes | 9 | 1 |
| memory, idle | 1 170 MiB | 31 MiB |
| memory, 10 000 entities | 2 141 MiB | 67 MiB |
| of it, the data service (Scorpio, PostGIS) | 349 / 1 323 MiB | |
| of it, the request path's security (APISIX, OPA, odrl-pap) | 76 / 74 MiB | |
| of it, what coraine stands for (the two above) | 425 / 1 397 MiB | 31 / 67 MiB |
| the store on disk | 126 MiB | 8 MiB |
| until the broker answers | 4.2 s | 0.54 s |

The TIL, the CCS, VCVerifier and their PostgreSQL (745 MiB idle) have no counterpart yet; those rows would move
with them. Since 10.11.0 the CCS (365 MiB idle) is disabled by default and VCVerifier serves its API. The
provider's Keycloak and DID Helper were not measured, on either side.

What the numbers say, and what they do not: they are one machine, one workload, and the connector's components
with their published defaults - not tuned. Scorpio's and APISIX's own scaling (more instances, more workers) is
not measured here; it adds processes and memory.

**Fewer moving parts** - one process where a data service and its protection are five today (the broker, its
database, APISIX, OPA, odrl-pap): fewer images to patch, fewer ports to secure, one log.

**Where it fits that today's does not** - 31 MiB idle and a start in half a second make a provider side
possible on an edge gateway or a small board, beside the devices whose data it serves.

## Where it stands next to the Eclipse Dataspace Connector

The Eclipse Dataspace Components (EDC) are the reference implementation of the Dataspace Protocol (DSP)
and the Decentralized Claims Protocol (DCP), which the Eclipse Foundation is submitting to ISO/IEC. Catena-X
requires a certified EDC of its participants; Simpl, Eona-X, the Mobility Data Space and parts of
Manufacturing-X build on it. The FDSC itself runs an EDC control plane (fdsc-edc).

A Cor DSC is not a competitor to that control plane, and does not propose to be one. Where it can add
something is beneath it:

- **The data plane.** DSP standardises the control plane; the data plane is left to the domain - the Data
  Spaces Support Centre's blueprint names "NGSI for smart cities" as an example, and EDC defines a Data Plane
  Signaling API so that a data plane can be written in another language. An NGSI-LD broker that enforces the
  agreed policies itself is such a data plane.
- **The footprint.** EDC's Tractus-X charts request 1 GiB and half a core for the control plane and as much
  for the data plane, plus PostgreSQL and Vault; we found no published EDC throughput figures. A native
  provider side is a different point on that curve - the edge, the city, the small participant.
- **NGSI-LD** is an ETSI standard and the FDSC's data API; a Cor DSC keeps it the native one.

What we do not know: whether the data spaces that require EDC would accept anything beneath an EDC control
plane but EDC's own data plane, and how certification would treat it.

## Open questions

- The connector's rules that `--auth fdsc` copies and that may not be intended (SEAMWARE/odrl-pap#59, #60,
  #61): the FDSC team decides; the Cor DSC follows.
- Per-entity and per-attribute decisions in a gateway (`--fdscNgsild` needs the stored entity): asking the
  broker, or deciding on the response.
- The credential formats, DIDs and trust lists a Cor verifier must accept to be a drop-in for VCVerifier -
  measured against the real one, as `test/fdscDiff` does for the decision.
