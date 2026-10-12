# Authorization in the broker

coraine decides **who sent a request and whether it may do what it asks** in the broker itself, through
an **auth plugin** - one at most, chosen with `--auth`. Without `--auth` every request is let in, as it
always has been, and none of this code runs. With `-DCOR_FEATURE_AUTH=OFF` it is not in the build.

Two parts:

- **the auth plugin contract** (§ 2) - a sixth plugin category, plain data across it, so that a
  deployment can bring its own security layer, written in any language that exports a C function;
- **`fdsc.so`** (§ 3) - the plugin coraine ships: the FIWARE Data Space Connector's provider-side
  security - VCVerifier's access tokens and the ODRL-PAP's policies - decided inside the broker, as the
  connector decides them today (§ 4).

## 1. Why in the broker

A security component in front of a broker re-reads every request: it parses the body again, guesses the
entity type from strings, and cannot see what the broker sees. In the Data Space Connector that component
is APISIX with an OPA sidecar, and the throughput a client gets is the proxy's, not the broker's.

Inside the broker the request is already parsed, the `@context` already applied, every entity type and
attribute name already expanded, a batch already split into its entities. A decision there costs a lookup,
not a second parse, and it can do what a proxy cannot (§ 6): decide per entity in a batch, decide on the
**stored** type of an entity whose request does not carry one, turn "may only read type X" into a
condition on the query itself, and see the tenant, the `@context` and the NGSI-LD operation.

## 2. The auth plugin contract

`--auth <name>` loads `<plugins>/auth/<name>.so` (or a path) and calls its `authRegister(AuthDriver*)`.
The contract is `src/lib/plugin/AuthDriver.h`, at `AUTH_ABI_VERSION` 2; append-only, as the bridge and
transport contracts are - the broker writes its version before `authRegister`, a plugin fills no slot the
broker is too old to have.

### 2.1 When the broker asks

| | when | the plugin answers | not allowed |
|---|---|---|---|
| `authenticate` | right after the route is looked up - **before** anything is done for the request: no 404/405/415 decided, no body parsed, no `@context` downloaded | who sent it: an identity handle of its own (NULL is one) | `AuthUnauthenticated` - **401**, `WWW-Authenticate` |
| `authorize` | once the `@context` is resolved, **before** the tenant is (resolving it creates it, for a create operation - a request that may not leaves nothing behind) | may it | `AuthDeny` - **403** |
| `entity` (ABI 2) | once `authorize` has let the request through, for **each entity** it reads or writes - see below | may it, this entity | `AuthDeny`: 403 for a one-entity operation, that entity's error in a batch's 207, left out of a query's response |
| `release` | the request is over, its response sent | - | - |

`entity` is given the entity's id, its **expanded** types and whether they are the **stored** entity's: the
types the body gives for a create (and for an upsert of a new entity); the stored entity's for a retrieve,
for every write on an existing entity - looked up before anything is forwarded to a context source, handed
to a bridge or written - and for each entity a query finds (`GET /entities`, `POST /entityOperations/query`,
the linked entities of `join=flat`, a Snapshot's entities read through `NGSILD-Snapshot` - the snapshot's,
on their own types). Refused, as not decided entity by entity: `join=inline` (its nested entities are not
asked about) and the EntityMaps themselves (`GET /entityMaps/{id}`, `POST /entityMaps` - a map names its
entities by id). A write that gives the entity types (a replace, a merge, a batch entry) is asked about
those too: changing or adding a type needs the right to it. An entity that is not stored here has no types.
A query's page may hold fewer entities than its `limit`, its position links (`pageAfter` / `pageBefore`)
name only entities the request may read, and its count counts the ones left out - with `q`, a count is an
answer about entities the request cannot see.

**Notifications** (ABI 2: `identitySave`, `identityRestore`). A subscription notifies long after the request
that made it, so the broker keeps its **owner** - who created it, or last updated it: `identitySave`'s text -
in the store's document collection `subscriptionOwners`, beside the subscription and never inside it
(nothing that renders a subscription can show it, and a broker that does not know the collection ignores
it). Each entity of each notification - the linked entities of a `join` too; `join=inline` is not done - is
asked about as a retrieve of it, with `entity` on the identity `identityRestore` makes of that text; the
ones the owner may not read are left out, and a notification left with none is not sent. An owner not
known (none kept, or one that no longer holds) leaves out every entity: fail closed. A PATCH of the
subscription makes whoever sent it the owner - how an owner takes a subscription back. A restored owner is
cached for 30 s: an owner a broker of an HA set stores is seen by the others within that, through the
shared store.

`AuthRequest.entities` says, per request, whether the broker will ask `entity` for each entity of this
operation (today: create, retrieve, every one-entity write, the attrs routes, the five batch writes, the
two queries) - a plugin leaves the entity's part of a decision to `entity` only when it is true.

Either may answer `AuthUnavailable` - **503**: the plugin cannot decide (no keys, no policies yet), and the
request is refused, never let in. The answers are ProblemDetails with type `about:blank` (RFC 9457
§ 4.2.1) - NGSI-LD defines no error type for either status - and the title and detail the plugin gives.

Every request is asked: HTTP, cor://, and **each message** of a transport (a WebSocket connection carries
no credentials, as it carries no tenant - each message has its own `Authorization` in its metadata; the
upgrade itself is not asked, a browser cannot set the header on it). Not asked: an in-process forward (its
request was asked when it arrived) and the broker's own requests - deleting the subscriptions of a closed
WebSocket connection. The health port (`--healthPort`) is a server of its own, outside the plugin's reach.

### 2.2 What crosses

Plain data only - `const char*`, integers, function pointers - as for a bridge: a plugin needs none of the
broker's trees or allocators. `AuthRequest` holds the method, the path, the URL parameters (decoded, in
order), the NGSI-LD operation (an `LdOp` bit and its name; 0 for a route that is no NGSI-LD operation),
the matched route, the tenant (`authorize`), and four accessors: `header(name)`, `body(&len)` - the body
**as the client sent it**, before any `@context` is applied - `expand(term)` with the request's
`@context`, and `expandDefault(term)` with the broker's default context (core, and `--defaultUserContext`) -
what a short name in a policy means, whatever the request's `@context` makes of it.

The plugin is called on the broker's request threads, concurrently, and with coroutines possibly on a
coroutine's stack: it is thread-safe and **never blocks** - keys and policies are fetched by threads of its
own.

### 2.3 Fail closed

A plugin that cannot be loaded, or whose `init` refuses its options, stops the broker (exit 1): `--auth`
asks for security, and starting without it would be starting open. A plugin's own options exist only when
it is loaded (`coraine --auth fdsc -u` lists them).

### 2.4 Writing one

```c
#include "plugin/AuthDriver.h"

static AuthVerdict authenticate(const AuthRequest* reqP, void** identityP, AuthAnswer* answerP)
{
  const char* key = reqP->header("X-Api-Key");

  if ((key == NULL) || (strcmp(key, "secret") != 0))
  {
    answerP->detail = "no API key";
    return AuthUnauthenticated;
  }

  return AuthAllow;
}

void authRegister(AuthDriver* driverP)
{
  driverP->abiVersion   = AUTH_ABI_VERSION;
  driverP->name         = "apikey";
  driverP->authenticate = authenticate;          // authorize NULL: an authenticated request may do anything
}
```

Built as a shared object, put in `<plugins>/auth/apikey.so`, run with `--auth apikey`. A plugin that calls
Cor-Lib functions is built against the same lib sources as its broker ([Plugin architecture](plugin-architecture.md)).

## 3. `fdsc.so` - the Data Space Connector's security

`--auth fdsc` takes what the connector's provider side has, unchanged:

- the **access token** VCVerifier issues for a Verifiable Presentation - checked here as APISIX's
  `openid-connect` checks it;
- the **ODRL policies** of the ODRL-PAP - evaluated here as OPA evaluates the Rego the PAP generates from them.

### 3.1 Options

| option | |
|---|---|
| `--fdscDiscovery <url>` | the verifier's OpenID discovery document - the URL APISIX's `openid-connect` has as `discovery` (`http://verifier:3000/services/<service>/.well-known/openid-configuration`). Its `jwks_uri` gives the keys, its `issuer` the `iss` a token must have |
| `--fdscJwks <url\|file>` | the JWK Set itself, instead of `--fdscDiscovery` |
| `--fdscIssuer <iss>` | the issuer a token must have - default: the discovery document's |
| `--fdscAudience <aud>` | an audience a token must have - default: not checked, as APISIX does not check it. VCVerifier sets `aud` to the service id |
| `--fdscLeeway <s>` | clock skew allowed on `exp` and `nbf` - default 120, what APISIX allows |
| `--fdscPap <url>` | the ODRL-PAP's base URL (`http://odrl-pap:8080`) - the policies are read from its `GET /policy` |
| `--fdscPolicies <url\|file>` | the policies themselves (the PAP's listing, an array of ODRL policies, one policy, or an `@graph`), instead of `--fdscPap` |
| `--fdscOrganization <did>` | the provider's DID - the PAP's `GENERAL_ORGANIZATION_DID`; `vc:role` reads a credential's roles for it |
| `--fdscRefresh <s>` | seconds between two fetches of keys and policies - default 30 |
| `--fdscStrippedPath` | `http:path` reads the `x-stripped-path` header, as the connector's Rego does - only behind a gateway that sets it and drops a client's; default: the request's path |
| `--fdscAnonymous` | a request without a token is let in as no one, instead of a 401 - only a `vc:any` policy can allow it anything |
| `--fdscNgsild` | decide on the entity, as NGSI-LD has it (§ 6) - **decisions differ from the connector's** |
| `--fdscOwnerMaxAge <s>` | `--fdscNgsild`: seconds a subscription's owner (its maker's credential, kept for its notifications) holds - after that, its notifications go without entities until the owner updates the subscription. 0 (default): until the credential's own `validUntil` (VC 1.1: `expirationDate`) |

A source of keys and a source of policies are required; without either the broker does not start.

### 3.2 The token

`Authorization: Bearer <JWT>`, the scheme in any case - as APISIX's `openid-connect` takes it with
`bearer_only` (no `X-Access-Token`, no exception for a CORS preflight). The token holds when:

- it is a compact JWS whose `alg` is **RS256 or ES256** - VCVerifier's two - and whose signature verifies
  with the key of its `kid` in the verifier's JWK Set (RSA, or EC P-256). `alg: none`, or an `alg` that is
  not its key's, is refused;
- `iss` is the issuer;
- `exp` has not passed by more than the leeway, nor `nbf` (when present) is still ahead by more than it;
- `aud` holds the audience, when one is configured.

A token that does not hold is a 401 with `WWW-Authenticate: Bearer error="invalid_token",
error_description="..."` (RFC 6750 § 3); no token, a 401 with `WWW-Authenticate: Bearer`.

The credential is the token's `verifiableCredential` (else `vc`) - what the PAP's Rego reads (§ 4).

`iss` is always checked: `--fdscJwks` needs `--fdscIssuer`, and with `--fdscDiscovery` a discovery document
without an `issuer` leaves every token refused (503). `aud` is checked with `--fdscAudience` - recommended
(VCVerifier sets the service id): without it, a token the verifier issued for another service is accepted,
as APISIX accepts it.

A token whose signature has been checked is kept (1024 of them): a client sends the same token with every
request for its 30 minutes, and a signature check costs as much as a request. A kept token is its exact
bytes; its claims are checked on every request. The kept tokens are dropped whenever the key set changes.

### 3.3 Keys and policies

A thread of the plugin fetches the keys (the discovery document, then `jwks_uri`) and the policies (the
PAP's `GET /policy`, page by page until a page is empty - the PAP says nothing of how many there are) every
`--fdscRefresh` seconds, and every second while either is missing. Each is replaced whole, only once it
has been read completely; a fetch that fails keeps what is in use.

- **No keys yet**: a request with a token is a 503; **no policies yet**: every request is a 503 - the
  connector answers 503 too while OPA has no bundle.
- **A token signed by a key the set does not have** - VCVerifier generates its key at every start - is a
  401 and has the keys fetched at once (not more than once a second): the client's next request with a
  new token is let in.

A policy that cannot be read (no `odrl:uid`, no permission, an unknown action or operator, an asset or party
collection without a refinement) is skipped and said in the log, as the PAP refuses it.

## 4. Compatible with the Data Space Connector

**Target: FIWARE Data Space Connector 10.87.0** (2026-10-09): VCVerifier 6.23.1, odrl-pap 1.7.1
(`SEAMWARE/odrl-pap`), odrl-authorization 2.4.3 (APISIX 3.17.0, OPA 1.11.0).

The plugin decides as that chain decides - for every request, the same 401, 403 or pass - so that a
deployment's tokens and policies need no change. That includes where the chain's Rego is not what ODRL
or NGSI-LD say - by default. The corrections are `--fdscNgsild` (§ 6), with decisions the connector
would not make.

### 4.1 The vocabulary

The words of the PAP's `mapping.json`, and what each reads:

| word | reads |
|---|---|
| `odrl:read` / `odrl:modify` / `odrl:delete` / `ngsild:create` / `odrl:use` | the method: GET / **PATCH only** / DELETE / POST / any of GET POST PUT PATCH DELETE. `dome-op:read\|modify\|write\|create`, `tmf:create` the same |
| assignee `vc:any` | anyone |
| assignee `<string>`, `odrl:Party` | the credential's **`issuer`** is that string |
| assignee `odrl:PartyCollection` | its refinement - and no issuer check |
| target `<uid>`, `odrl:Asset` | the last path segment is the uid |
| target `odrl:AssetCollection` | its refinement |
| `ngsi-ld:entityType`, `ngsi-ld:entityTypeGroup` | the third `:` part of the last path segment (`urn:ngsi-ld:<Type>:...`), else the `type` URL parameter, else the body's `type` - as sent, never expanded |
| `vc:role` | `credentialSubject.roles[]` whose `target` is the organisation: its `names` |
| `vc:type` | the credential's `type` (a string as a one-element array) |
| `vc:currentParty`, `dome-op:currentParty` | the credential's `issuer` |
| `http:path` | the path (the connector: the `x-stripped-path` header first - with `--fdscStrippedPath`, § 4.4) |
| `http:bodyValue "$.a.b"` | the body's value at `a.b`, as sent |
| `ovc:leftOperand "a.b"` | the credential's value at `a.b`; `ovc:credentialSubjectType` its `credentialSubject.type` |
| `odrl:dateTime` / `odrl:dayOfWeek` / `odrl:hourOfDay` | now in ms / 0 = Monday, UTC / 0-23, UTC |
| `dome-op:role` | `credentialSubject.rolesAndDuties[]` for the organisation: its `roleNames` |
| `tmf:lifecycleStatus` / `tmf:resource` | the body's `lifeCycleStatus` / the last path segment without "ngsi-ld" |

Operators: `eq`, `neq`, `gt`, `gteq`, `lt`, `lteq` (Rego's order across types: null < boolean < number <
string < array < object), `hasPart` and `isPartOf` (both: the right operand is in the left), `isAnyOf`,
`isNoneOf`, `isAllOf` (equality), `regex` (POSIX extended, unanchored), `http:isInPath` (the right path a
prefix of the left, segment by segment, `*` one any segment). Logical constraints `and`, `andSequence`,
`or`, `xone`. Right operands: a string, a number, a list, `{"@value", "@type"}` with `xsd:integer` and its
kin (a number), `xsd:decimal`/`float`/`double`, `xsd:date` (ms at 00:00 UTC); `odrl:policyUsage` (now).

Words of the PAP's other PEPs (`json:*`, `dspace:read`) and those that make a call of their own
(`dome-op:relatedParty`, `owner`, `validFor_*`) read nothing an HTTP request has: never true.

### 4.2 How a decision is made

- A request is allowed when **one** policy allows it; there is no default allow, and no policy at all
  allows nothing.
- A policy allows when **all** its rules hold - the rules of **all its permissions** together: the PAP
  puts them in one rule set, so two permissions are an AND, not ODRL's OR. Prohibitions, obligations and
  duties are ignored, as the PAP ignores them.
- A rule on a value the request or the credential does not have is not true - nor its negation
  (`neq`, `isNoneOf`), nor a logical constraint that holds it (Rego's undefined).
- A token with two or more credentials (`verifiablePresentation`) satisfies no `vc:` word and no issuer
  assignee: the PAP's Rego reads `verifiableCredential` only.
- A credential with two different role lists for the organisation is OPA's `eval_conflict_error`: 503.

### 4.3 Proven against the connector itself

`test/fdscDiff/fdscDiff.sh` deploys the connector's chain - APISIX 3.17.0 with `openid-connect` and
odrl-authorization 2.4.3's `opa` plugin (fetched from its tag), OPA 1.11.0, odrl-pap 1.7.1 on PostgreSQL -
in docker, beside coraine with `--auth fdsc`, both given the same verifier (a stand-in serving the
discovery document and the JWK Set as VCVerifier does) and the same PAP. Each case of
`test/fdscDiff/cases.json` - tokens, the vocabulary, the combination rules, the cases where the Rego is not
what ODRL says - goes to both, and the answers are compared by class (401, 403, 503, let through).
**59 cases, 0 differences** (2026-10-12), apart from the one decided below (`known.json`).

The functests (`auth_fdsc_*`, `auth_startup_refused`) hold the same behaviour without docker, against a
stand-in PAP (`test/funcTests/tools/ftVerifier.py`).

### 4.4 Where coraine decides otherwise, on purpose

Where the connector's chain does not work at all, coraine does not copy the failure:

- **ES256.** APISIX's `openid-connect` (lua-resty-openidc 1.8.0) cannot use an EC key of a JWK Set that
  has no `x5c` ("No trusted certs loaded"), and VCVerifier's has none: behind APISIX a verifier configured
  for ES256 gets every token refused. coraine verifies it.
  ([FIWARE/data-space-connector#238](https://github.com/FIWARE/data-space-connector/issues/238))
- **`x-stripped-path`.** The PAP's `http:path` reads that header before the path, and APISIX passes a
  client's on: a client sends `x-stripped-path: /allowed/path` and an `http:isInPath` rule holds for any
  request. coraine reads the request's path, and the header only with `--fdscStrippedPath` - behind a
  gateway that sets it and drops the client's.
- **A policy whose Rego does not parse.** The PAP accepts `"odrl:leftOperand": "http:bodyValue"` (or
  `ovc:leftOperand`) without the path - the key form `"http:bodyValue": "$.a.b"` is the one that works - and
  generates Rego with the path's `%s` left in it. OPA then refuses the **whole** policies bundle: every
  request of the deployment is denied, whatever the other policies say. coraine skips that one policy, says
  so in its log, and decides with the others.
  ([SEAMWARE/odrl-pap#58](https://github.com/SEAMWARE/odrl-pap/issues/58))

## 5. Inside a Data Space Connector

The provider's data route goes to coraine instead of through APISIX's `openid-connect` and `opa` plugins:

| APISIX route (`k3s/provider.yaml`) | coraine |
|---|---|
| `openid-connect.discovery` | `--fdscDiscovery` (same URL) |
| `openid-connect.bearer_only`, `use_jwks` | what `fdsc.so` does |
| `opa.policy: policy/main`, `with_body: true` | `--fdscPap http://odrl-pap:8080` |
| PAP `GENERAL_ORGANIZATION_DID` | `--fdscOrganization` |

VCVerifier, the Trusted Issuers List, the Credentials Config Service and the ODRL-PAP stay as they are -
the PAP is still where policies are written; coraine reads them where OPA did. Nothing on the consumer
side changes. The connector also routes `GET /.well-known/openid-configuration` on the data host to the
verifier, so that a client finds the token endpoint: that route stays on the proxy (or ingress) in front.

## 6. `--fdscNgsild` - decided on the entity

What the connector's chain gets wrong for NGSI-LD, from the PAP's `ngsi-ld/leftOperand.rego` and
`utils/apisix.rego` - and what `fdsc.so` does instead with `--fdscNgsild`, on the broker's per-entity
decisions (`entity`, § 2.1). The policies are the same; what their words read is not:

| | the connector (default) | `--fdscNgsild` |
|---|---|---|
| `ngsi-ld:entityType` | the third `:` part of the last path segment (`urn:ngsi-ld:<Type>:...`), else `?type=` as one string, else the body's `type` - as sent | the entity's types: the body's for a create, the **stored** entity's for everything else - **expanded**; the policy's type expanded with the broker's default context. A positive operator holds when it holds for one of the types, `neq` / `isNoneOf` when for all |
| an id that does not say its type, `/attrs/{attr}` | no type: denied | the stored type |
| a write on `/entities/{id}/attrs` whose body has a `type` | the body's - the last path segment is `attrs`: the client chooses the type | the stored type |
| an id that says another type than the entity's | the id's | the entity's |
| a request `@context` that makes the word another type | the word matches | it does not |
| a batch | one `type` sought in an array body - none: denied | entity by entity - a denied one is its 403 in the 207 |
| a query | `?type=A,B` is one string; a POST query has no `type` | each entity found, on its stored type - the denied ones left out |
| the action | the HTTP method: `odrl:modify` is PATCH only, a POST query is a create | the NGSI-LD operation: a POST query is a read, a PUT a modification, an upsert per entity a create or an update |
| notifications | not decided: a subscription notifies what matches | for the subscription's owner, entity by entity (§ 2.1); the owner's credential kept in the store, honoured until its `validUntil` or `--fdscOwnerMaxAge` |

For an operation the broker does not decide entity by entity (`AuthRequest.entities` false - the temporal
API, subscriptions, registrations, `/types`, ...), `--fdscNgsild` reads the entity type as the connector does.

`auth_fdsc_ngsild_entity`, `auth_fdsc_ngsild_batch`, `auth_fdsc_ngsild_query` and `auth_fdsc_ngsild_notify` hold
it; each of them fails without `--fdscNgsild`.

A credential kept as an owner is personal data at rest: it goes with its subscription (a DELETE of it), and
it is never checked again for revocation - VCVerifier checks that when it issues a token. `--fdscOwnerMaxAge`
bounds how long a revoked credential still receives notifications.

## 7. Next

- A subscription's owner kept in the store with **haaux** (corDB HA): the owners are not among what haaux
  syncs - with MongoDB the store is shared, and they are.
- The tenant (`NGSILD-Tenant`) is never examined by the connector's policies.
- A request's `@context` (its `Link` header, its body's `@context`) is fetched before `authorize`: an
  authenticated client - anyone, with `--fdscAnonymous` - can have the broker fetch a URL of its choice,
  as it can through the connector's chain, which forwards the request to its broker. Deciding first needs
  `authorize` without the expanded request.
- Attribute-level rules (read and write per attribute: `AuthEntity.attrV`); serving the PAP's `/policy`
  API from the broker.

## Sources (2026-10-12)

- `FIWARE/data-space-connector` 10.87.0: `k3s/provider.yaml`, `charts/data-space-connector/Chart.yaml`,
  `it/src/test/resources/policies/*.json`, `doc/deployment-integration/local-deployment/LOCAL.MD`
- `FIWARE/odrl-authorization` 2.4.3: `charts/odrl-authorization` (`files/apixis-opa.lua`, `files/apixis-helper.lua`,
  `templates/opa-cm.yaml`, `templates/routes.yaml`)
- `SEAMWARE/odrl-pap` 1.7.1: `src/main/resources/mapping.json`, `rego/ngsi-ld/*.rego`, `rego/vc/*.rego`,
  `rego/odrl/*.rego`, `rego/utils/apisix.rego`, `mapping/OdrlMapper.java`, `mapping/MappingResult.java`,
  `BundleResource.java`, `api/odrl.yaml`
- `FIWARE/VCVerifier` 6.23.1: `verifier/verifier.go`, `openapi/routers.go`, `openapi/api_api.go`, `config/config.go`
- `apache/apisix` 3.17.0 `apisix/plugins/openid-connect.lua`; `zmartzone/lua-resty-openidc` 1.8.0
