# A Cor Data Space Connector - a proposal

An alternative provider side for the FIWARE Data Space Connector (FDSC), built on coraine and the cor
libraries: the same tokens, the same ODRL policies, the same NGSI-LD API, the same consumer side, in a
fraction of the processes and the memory. A proposal, not a plan: nothing below is scheduled.

## What the provider side is today

The FDSC's provider side, as its charts deploy it (data-space-connector 10.10.3):

| role | today |
|---|---|
| the data | Scorpio, an NGSI-LD broker, on PostgreSQL/PostGIS |
| who sent a request | APISIX's `openid-connect`, checking the access token VCVerifier issued |
| may it | OPA, running the Rego odrl-pap writes from the ODRL policies, asked by APISIX's `opa` plugin |
| the policies | odrl-pap, on PostgreSQL |
| the token | VCVerifier: a verifiable credential presented (OID4VP), exchanged for a JWT |
| which issuers are trusted | the Trusted Issuers List (TIL), on PostgreSQL |
| which credentials a service takes | the Credentials Config Service (CCS), on PostgreSQL |
| the Dataspace Protocol | fdsc-edc: an Eclipse Dataspace Components (EDC) control plane with FIWARE's extensions, listed among the DSP-compliant connectors in IDSA's Data Space Connector Report No. 2 (June 2026) |
| contracts, the marketplace | TM Forum APIs, contract management, the Business API Ecosystem |

## What a Cor Data Space Connector would be

| role | a Cor DSC | state |
|---|---|---|
| the data | coraine | built |
| who sent a request, may it | `--auth fdsc` in the broker ([Authorization](authorization.md)) - VCVerifier's tokens, the ODRL policies as odrl-pap holds them, decided as the connector decides them | built; 64 cases of `test/fdscDiff`, 0 differences against the connector's own chain |
| the policies | coraine's `/policy` API (`--policyApiPort`): odrl-pap's API, the policies in coraine's store | built |
| the same decision in APISIX, for any NGSI-LD broker | `ngsild-pep` (corApisixNgsild): `fdsc.so`'s code in an APISIX plugin | built; the same 64 cases, 0 differences |
| the token | a verifier: OID4VP, the credential formats and DIDs the connector accepts, the JWT | not started |
| which issuers are trusted | a TIL, with the EBSI TIR API the verifier reads | not started |
| which credentials a service takes | a CCS | not started |
| the Dataspace Protocol | fdsc-edc as it is | kept |
| contracts, the marketplace, the consumer side | as they are | kept |

What is built covers every request to the data. What is not started is the authentication side: the verifier,
the TIL and the CCS (in FIWARE's code: 0.9 MB of Go of its own plus the Trustbloc libraries, 0.2 MB and 0.1 MB
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

**Footprint** - the provider side in containers as its charts deploy it, against one coraine (the broker,
`--auth fdsc`, `/policy`, durable store), with the same 10 000 entities:

| | today's provider side | coraine |
|---|---:|---:|
| processes | 9 | 1 |
| memory, idle | 1 170 MiB | 31 MiB |
| memory, 10 000 entities | 2 141 MiB | 67 MiB |
| of it, what coraine replaces (broker, gateway, OPA, PAP, databases) | 494 / 1 468 MiB | 31 / 67 MiB |
| the store on disk | 126 MiB | 8 MiB |
| until the broker answers | 4.2 s | 0.54 s |

The TIL, the CCS and VCVerifier (676 MiB idle) have no counterpart yet; those rows would move with them.

What the numbers say, and what they do not: they are one machine, one workload, and the connector's components
with their published defaults - not tuned. Scorpio's and APISIX's own scaling (more instances, more workers) is
not measured here; it adds processes and memory.

**Fewer moving parts** - one process where the request path has six today (Scorpio, its database, APISIX,
OPA, odrl-pap, its database): fewer images to patch, fewer ports to secure, one log.

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
