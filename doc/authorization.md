# Authorization in the broker

coraine decides **who sent a request and whether it may do what it asks** in the broker itself, through
an **auth plugin** - one at most, chosen with `--auth`. Without `--auth` every request is let in, as it
always has been, and none of this code runs. With `-DCOR_FEATURE_AUTH=OFF` it is not in the build.

Three parts:

- **the auth plugin contract** (§ 2) - a sixth plugin category, plain data across it, so that a
  deployment can bring its own security layer, written in any language that exports a C function;
- **`fdsc.so`** (§ 3) - the FIWARE Data Space Connector's provider-side security - VCVerifier's access
  tokens and the ODRL-PAP's policies - decided inside the broker, as the connector decides them today (§ 4);
- **`oidc.so`** (§ 8) - an OpenID provider's access tokens (Keycloak, Auth0, Entra ID, ...) and two roles
  read out of them: one reads, one reads and writes.

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
The contract is `src/lib/plugin/AuthDriver.h`, at `AUTH_ABI_VERSION` 6; append-only, as the bridge and
transport contracts are - the broker writes its version before `authRegister`, a plugin fills no slot the
broker is too old to have.

### 2.1 When the broker asks

| | when | the plugin answers | not allowed |
|---|---|---|---|
| `authenticate` | right after the route is looked up - **before** anything is done for the request: no 404/405/415 decided, no body parsed, no `@context` downloaded | who sent it: an identity handle of its own (NULL is one) | `AuthUnauthenticated` - **401**, `WWW-Authenticate` |
| `authorize` | with `authorizeBeforeContext` (ABI 3; `fdsc.so` sets it): right after `authenticate`, **before** the request's `@context` is resolved - its `Link` header and its body's `@context` name URLs the client chooses, and the broker fetches them; `expand()` answers NULL there. Without it: once the `@context` is resolved. Either way **before** the tenant is resolved (resolving it creates it, for a create operation - a request that may not leaves nothing behind) | may it | `AuthDeny` - **403** |
| `entity` (ABI 2) | once `authorize` has let the request through, for **each entity** it reads or writes - see below | may it, this entity - for a write, with the **attributes** it writes (`attrV`: the body's, or the path's `{attr}`); for a read of the entity whole, `AuthAnswer.partial` (ABI 4): only some of its attributes - the broker then asks about each attribute alone and leaves out the ones not allowed | `AuthDeny`: 403 for a one-entity operation, that entity's error in a batch's 207, left out of a query's response |
| `notify` (ABI 5) | for each notification of a subscription, once, before any `entity` - on its owner's identity (see below) | may the owner be sent it at all | not `AuthAllow`: the notification is not sent |
| `resourceMetadata` (ABI 6) | for `GET /.well-known/oauth-protected-resource`, and for every 401 | how a client gets a token it takes: a JSON object (§ 2.5) | - |
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
shared store (HA runs on MongoDB - [High Availability](high-availability.md)). A plugin with `notify`
(ABI 5) is asked first, once per notification, with the owner and the subscription's tenant: not allowed,
or no owner known, and the notification is not sent. A plugin that decides notifications as a whole sets
`notify` and no `entity`: the broker then asks nothing per entity - of a notification, or of a request.

`AuthRequest.entities` says, per request, whether the broker will ask `entity` for each entity of this
operation (today: create, retrieve, every one-entity write, the attrs routes, the five batch writes, the
two queries, and the temporal API - retrieve, query, the upsert, the attribute and instance writes, the
delete) - a plugin leaves the entity's part of a decision to `entity` only when it is true.

An entity's stored types are its current state's; for one that has history and no current state (a
deleted entity, one made through the temporal API), its history's.

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

A plugin whose `authorize` reads nothing of the request's own `@context` (no `expand()`) sets
`driverP->authorizeBeforeContext = true` (ABI 3): a request it refuses is then refused before the broker fetches
the `@context` URLs the client named.

### 2.5 How a client gets a token - the resource's metadata

A plugin with `resourceMetadata` (ABI 6) says how a client gets a token it takes - OAuth 2.0 Protected Resource
Metadata (RFC 9728). The broker serves it, **without authentication**, at `GET
/.well-known/oauth-protected-resource`: the plugin's members - `authorization_servers` (the issuers), and its
own (`coraine_auth`: which plugin) - and `resource` (`--httpEndpoint`) and `bearer_methods_supported`
(`["header"]`) where the plugin gives none. Every **401** names it, after the plugin's own auth-params (§ 5.1):

```
WWW-Authenticate: Bearer error="invalid_token", error_description="wrong issuer", resource_metadata="http://broker:1026/.well-known/oauth-protected-resource"
```

A client - c³ - reads it on the first 401 and knows where to log in. Without `--auth`, or with a plugin that
offers none, the route is a 404. `COR_FEATURE_AUTH_METADATA` (on; off with `COR_FEATURE_AUTH` off), about
1 KB of the broker.

| plugin | its members |
|---|---|
| `oidc.so` | `authorization_servers`: `--oidcIssuer`; `scopes_supported`: `["openid"]`; `coraine_roles`: the claim and the read and write roles; `coraine_tenants`: the tenants claim (`null` with `--oidcAnyTenant`) |
| `fdsc.so` | `authorization_servers`: the verifier's issuer (`--fdscIssuer`, or its discovery document's); `fdsc_discovery`: `--fdscDiscovery`, when given; `fdsc_audience`: `--fdscAudience`, when given |

## 3. `fdsc.so` - the Data Space Connector's security

`--auth fdsc` takes what the connector's provider side has, unchanged:

- the **access token** VCVerifier issues for a Verifiable Presentation - checked here as APISIX's
  `openid-connect` checks it;
- the **ODRL policies** of the ODRL-PAP - evaluated here as OPA evaluates the Rego the PAP generates from them.

### 3.1 Options

The options, as [Command-line options](options.md#plugin-fdsc-loaded-by-auth-fdsc) has them:

<!-- options of the fdsc plugin: copied from doc/options.md by tools/pluginOptions.sh - edit them there -->

#### `--fdscDiscovery`

`-fdscDiscovery` · `CORAINE_FDSCDISCOVERY` · String · default: no default - `--fdscJwks` is needed instead
· values: a URL (`http://`, `https://`), or a file (a path, or `file://`) · plugin: fdsc

The verifier's OpenID discovery document - the URL APISIX's `openid-connect` has as `discovery`
(`http://verifier:3000/services/<service>/.well-known/openid-configuration`). Its `jwks_uri` gives the keys,
its `issuer` the `iss` a token must have unless `--fdscIssuer` is given. A document without an `issuer`, and
no `--fdscIssuer`: every request with a token is a 503
(`the verifier's issuer is not known (its discovery document has none, and --fdscIssuer is not set)`).
Without it and without `--fdscJwks` the broker stops at start:
`auth plugin 'fdsc': no keys to check a token with: --fdscDiscovery or --fdscJwks`. With both,
`--fdscJwks` is used and the discovery document is not read.

    coraine --auth fdsc --fdscDiscovery http://verifier:3000/services/data-service/.well-known/openid-configuration \
            --fdscPap http://odrl-pap:8080

#### `--fdscJwks`

`-fdscJwks` · `CORAINE_FDSCJWKS` · String · default: no default - the keys come from `--fdscDiscovery`
· values: a URL (`http://`, `https://`), or a file (a path, or `file://`) · plugin: fdsc

The verifier's JWK Set itself, instead of the discovery document. It needs `--fdscIssuer`: the broker stops
at start with `auth plugin 'fdsc': --fdscJwks needs --fdscIssuer: a token's iss is always checked`. A JWK
Set that cannot be read is in the log, and requests with a token are a 503 until it can
(`the verifier's keys are not loaded yet`).

    coraine --database corDB --auth fdsc --fdscJwks /etc/coraine/jwks.json --fdscIssuer did:web:verifier.example.org \
            --fdscPolicies /etc/coraine/policies.json

#### `--fdscIssuer`

`-fdscIssuer` · `CORAINE_FDSCISSUER` · String · default: no default - the discovery document's `issuer`
· values: the verifier's issuer (a URL or a DID) · plugin: fdsc

The `iss` every token must have. Without it, the one the discovery document gives (re-read at every
refresh). Required with `--fdscJwks`. A token of another issuer is a 401 with
`error_description="wrong issuer"`. It is what `GET /.well-known/oauth-protected-resource` gives as
`authorization_servers`.

    coraine --auth fdsc --fdscJwks /etc/coraine/jwks.json --fdscIssuer did:web:verifier.example.org --fdscPap http://odrl-pap:8080

#### `--fdscAudience`

`-fdscAudience` · `CORAINE_FDSCAUDIENCE` · String · default: no default - `aud` is not checked
· values: an audience - the verifier's service id · plugin: fdsc

An audience the token's `aud` must hold. VCVerifier sets `aud` to the service id. Without it a token the
verifier issued for another service is accepted - as APISIX accepts it; with it, such a token is a 401 with
`error_description="wrong audience"`. When given, `GET /.well-known/oauth-protected-resource` has it as
`fdsc_audience`.

    coraine --auth fdsc --fdscDiscovery http://verifier:3000/services/data-service/.well-known/openid-configuration \
            --fdscAudience data-service --fdscPap http://odrl-pap:8080

#### `--fdscLeeway`

`-fdscLeeway` · `CORAINE_FDSCLEEWAY` · Int · default: `120`
· values: 0-600 (seconds) · plugin: fdsc

The clock skew allowed on a token's `exp` and `nbf`, in seconds: a token is taken until `exp` plus the
leeway, and from `nbf` minus it. 120 is what APISIX's `openid-connect` (lua-resty-openidc) allows. A token
outside it is a 401 with `error_description="expired, or not yet valid"`; a token from the token cache is
checked again on every request.

    coraine --auth fdsc --fdscDiscovery http://verifier:3000/services/data-service/.well-known/openid-configuration \
            --fdscPap http://odrl-pap:8080 --fdscLeeway 30

#### `--fdscPap`

`-fdscPap` · `CORAINE_FDSCPAP` · String · default: no default - `--fdscPolicies` is needed instead
· values: the ODRL-PAP's base URL · plugin: fdsc

The ODRL-PAP whose policies decide (`http://odrl-pap:8080`). They are read from its `GET /policy`, page by
page (`?page=<n>&pageSize=100`) until a page is empty, and installed only when every page has been read: a
fetch that fails half-way keeps the policies in use. A policy that cannot be read is skipped and said in the
log. Without it and without `--fdscPolicies` the broker stops at start:
`auth plugin 'fdsc': no policies to decide with: --fdscPap or --fdscPolicies`. With both, `--fdscPolicies`
is used.

The broker's own policy API (`--policyApiPort`) is such a PAP: `--fdscPap http://127.0.0.1:<port>`.

    coraine --database corDB --policyApiPort 8081 --auth fdsc --fdscPap http://127.0.0.1:8081 \
            --fdscJwks /etc/coraine/jwks.json --fdscIssuer did:web:verifier.example.org

#### `--fdscPolicies`

`-fdscPolicies` · `CORAINE_FDSCPOLICIES` · String · default: no default - `--fdscPap` is needed instead
· values: a URL (`http://`, `https://`), or a file (a path, or `file://`) · plugin: fdsc

The ODRL policies themselves, instead of a PAP: a JSON array - of ODRL policies, or the PAP's own listing
(`[{"id", "odrl:uid", "odrl": "<the policy as a string>"}]`) - a single policy object, or an object whose `@graph`
holds the policies. Read again every
`--fdscRefresh` seconds. A file that is not JSON leaves the policies in use (none at first: every request
is a 503, `the policies are not loaded yet`).

    coraine --database corDB --auth fdsc --fdscJwks /etc/coraine/jwks.json --fdscIssuer did:web:verifier.example.org \
            --fdscPolicies /etc/coraine/policies.json

#### `--fdscOrganization`

`-fdscOrganization` · `CORAINE_FDSCORGANIZATION` · String · default: no default - the PAP's default DID, `urn:ngsi-ld:organization:0b03975e-7ded-4fbd-9c3b-a5d6550df7e2`
· values: a DID · plugin: fdsc

The provider's DID - the PAP's `GENERAL_ORGANIZATION_DID`. A policy's `vc:role` reads the roles the
credential's subject has for this organisation: the `names` of the entry of `credentialSubject.roles` whose
`target` is this DID. A credential whose roles are for another target holds no role here. Two entries for
the organisation with different names are OPA's `eval_conflict_error`: a 503. Set it to what the PAP has.

    coraine --auth fdsc --fdscDiscovery http://verifier:3000/services/data-service/.well-known/openid-configuration \
            --fdscPap http://odrl-pap:8080 --fdscOrganization did:web:provider.example.org

#### `--fdscRefresh`

`-fdscRefresh` · `CORAINE_FDSCREFRESH` · Int · default: `30`
· values: 1-86400 (seconds) · plugin: fdsc

Seconds between two fetches of the keys and the policies. While either is missing, both are fetched every
second, whatever the option says - the verifier and the PAP may start after the broker. A token signed with
a key the set does not have (VCVerifier makes a new key at every start) is a 401 and has the keys fetched at
once, at most once a second. A policy written to the PAP is in use within this many seconds. A new key set
empties the token cache.

    coraine --auth fdsc --fdscDiscovery http://verifier:3000/services/data-service/.well-known/openid-configuration \
            --fdscPap http://odrl-pap:8080 --fdscRefresh 5

#### `--fdscAnonymous`

`-fdscAnonymous` · `CORAINE_FDSCANONYMOUS` · Bool · default: `false`
· values: a flag · plugin: fdsc

A request without a token is not a 401: it is let in as no one, and the policies decide it - only a policy
whose assignee is `vc:any` can allow it anything; for the rest it is a 403. A request with a token that does
not hold is still a 401.

    coraine --database corDB --auth fdsc --fdscJwks /etc/coraine/jwks.json --fdscIssuer did:web:verifier.example.org \
            --fdscPolicies /etc/coraine/policies.json --fdscAnonymous

#### `--fdscNgsild`

`-fdscNgsild` · `CORAINE_FDSCNGSILD` · Bool · default: `false`
· values: a flag · plugin: fdsc

Decide on the entity, as NGSI-LD has it, instead of as the Data Space Connector does - so decisions differ
from the connector's. `ngsi-ld:entityType` is then the entity's own types, expanded: the body's for a
create, the stored entity's for everything else; each entity of a batch on its own (a denied one is its 403
in the 207), each entity of a query on its own (the denied ones left out); the action is the NGSI-LD
operation, not the HTTP method. Without it, `ngsi-ld:entityType` is what the connector reads: the type part
of the id in the path, else `?type=` as one string, else the body's `type`.

Only with it: the words `ngsi-ld:tenant` and `ngsi-ld:attribute` (a policy that uses one is skipped without
it, and the log says why:
`policy '<uid>' skipped: 'ngsi-ld:tenant' is coraine's own word - only with --fdscNgsild`); and the
notifications - decided for the subscription's owner, entity by entity, the owner's credential kept with the
subscription (`--fdscOwnerMaxAge`). Without it a subscription notifies what matches, and no credential is
kept. Not available in the APISIX plugin: it needs the stored entity
([Authorization](authorization.md#6---fdscngsild---decided-on-the-entity)).

    coraine --database corDB --auth fdsc --fdscJwks /etc/coraine/jwks.json --fdscIssuer did:web:verifier.example.org \
            --fdscPolicies /etc/coraine/policies.json --fdscNgsild

#### `--fdscOwnerMaxAge`

`-fdscOwnerMaxAge` · `CORAINE_FDSCOWNERMAXAGE` · Int · default: `0` (until the credential's own end)
· values: 0-31536000 (seconds) · plugin: fdsc

With `--fdscNgsild` only (without it no owner is kept, and the option changes nothing). A subscription's
owner - the credential of the token that made or last updated it, kept for its notifications - holds for
this many seconds from that request; after that its notifications go without entities, until the owner
updates the subscription. 0: until the credential's own `validUntil` (VC 1.1: `expirationDate`); a
credential past it no longer holds whatever this option says. A kept credential is never checked again for
revocation: this is what bounds how long a revoked one still receives notifications.

    coraine --database corDB --auth fdsc --fdscJwks /etc/coraine/jwks.json --fdscIssuer did:web:verifier.example.org \
            --fdscPolicies /etc/coraine/policies.json --fdscNgsild --fdscOwnerMaxAge 86400

#### `--fdscStrippedPath`

`-fdscStrippedPath` · `CORAINE_FDSCSTRIPPEDPATH` · Bool · default: `false`
· values: a flag · plugin: fdsc

`http:path` reads the `x-stripped-path` header when the request has one, as the connector's Rego does;
without the option it reads the request's own path, and the header is ignored. Only behind a gateway that
sets the header and drops a client's: a client that can send it chooses the path an `http:isInPath` rule
sees ([Authorization](authorization.md#44-where-coraine-decides-otherwise-on-purpose)).

    coraine --auth fdsc --fdscDiscovery http://verifier:3000/services/data-service/.well-known/openid-configuration \
            --fdscPap http://odrl-pap:8080 --fdscStrippedPath

<!-- end of the options of the fdsc plugin -->

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

The token's size is the client's, and a cost of every request: the broker receives the whole header before
anything reads it - a 696-byte token, 0.31-0.86 µs of a 10 µs PATCH on one core
([The token's size](performance.md#the-tokens-size)). With `--auth`, the broker says once in its log when
requests carry an `Authorization` header over 512 bytes.

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

**Target: FIWARE Data Space Connector 10.87.0** (2026-10-09): VCVerifier 6.23.1, odrl-authorization 2.4.3
(APISIX 3.17.0, OPA 1.11.0), and odrl-pap **1.7.3** (`SEAMWARE/odrl-pap`, 2026-10-14) - the connector's
chart pins 1.7.1; 1.7.2 refuses a policy whose left operand lacks its argument, 1.7.3 finds the entity type
of a batch, and the plugin decides as 1.7.3.

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
| `ngsi-ld:entityType`, `ngsi-ld:entityTypeGroup` | the third `:` part of the last path segment (`urn:ngsi-ld:<Type>:...`), else the `type` URL parameter, else the body's `type` - as sent, never expanded -, else, for a batch (an array of entities), the one type all of them have: none when they differ, one has no `type` or one that is no string, or the array is empty (odrl-pap 1.7.3) |
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
  allows nothing. The policies are evaluated in the PAP's order - the order of the `allow` rules of OPA's
  `policy.main` - and the first that allows decides: OPA's early exit evaluates none after it.
- A policy allows when **all** its rules hold - the rules of **all its permissions** together: the PAP
  puts them in one rule set, so two permissions are an AND, not ODRL's OR. Prohibitions, obligations and
  duties are ignored, as the PAP ignores them.
- A rule on a value the request or the credential does not have is not true - nor its negation
  (`neq`, `isNoneOf`), nor a logical constraint that holds it (Rego's undefined).
- A token with two or more credentials (`verifiablePresentation`) satisfies no `vc:` word and no issuer
  assignee: the PAP's Rego reads `verifiableCredential` only.
- A credential with two different role lists for the organisation is OPA's `eval_conflict_error` - 503 -
  where a policy evaluated before the first that allows reads `vc:role` (or `dome-op:role`). Which rule
  of one policy is evaluated first is the PAP's: it writes a policy's rules in an order of its own (the
  assignee first in one policy, the target first in another), and coraine evaluates the action, then the
  assignee, then the target. For such a credential, and only for one, a policy whose `vc:role` rule the
  PAP writes after a rule that fails can be a 503 here and not in the connector.

### 4.3 Proven against the connector itself

`test/fdscDiff/fdscDiff.sh` deploys the connector's chain - APISIX 3.17.0 with `openid-connect` and
odrl-authorization 2.4.3's `opa` plugin (fetched from its tag), OPA 1.11.0, odrl-pap 1.7.3 on PostgreSQL -
in docker, beside coraine with `--auth fdsc`, both given the same verifier (a stand-in serving the
discovery document and the JWK Set as VCVerifier does) and the same PAP. Each case of
`test/fdscDiff/cases.json` - tokens, the vocabulary, the combination rules, the cases where the Rego is not
what ODRL says - goes to both, and the answers are compared by class (401, 403, 503, let through).
**64 cases, 0 differences** (2026-10-13), apart from the one decided below (`known.json`).

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
- **A policy whose left operand lacks its argument.** `"odrl:leftOperand": "http:bodyValue"` (or
  `ovc:leftOperand`) without the path - the key form `"http:bodyValue": "$.a.b"` is the one that works.
  odrl-pap 1.7.1 accepted it and generated Rego with the path's `%s` left in it, and OPA then refused the
  **whole** policies bundle: every request of the deployment denied. Since 1.7.2 the PAP refuses the policy
  (its message: "The leftOperand `http:bodyValue` requires 1 parameter(s)..."; the status it answers is a
  500). coraine's `/policy` API refuses it too, with that message and a 400; a policy file or an older PAP
  that has one: coraine skips that one policy, says so in its log, and decides with the others.
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

For a broker other than coraine, the same decision is made in APISIX: the plugin `ngsild-pep`
([corApisixNgsild](https://github.com/SEAMWARE/corApisixNgsild)) runs `fdsc.so`'s code in the gateway,
the token check included - it takes the place of the route's `openid-connect` and `opa`. `--fdscNgsild`
(§ 6) is not available there: it needs the stored entity, which only the broker has.

<a id="6---fdscngsild---decided-on-the-entity"></a>

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

For an operation the broker does not decide entity by entity (`AuthRequest.entities` false - subscriptions,
registrations, `/types`, ...), `--fdscNgsild` reads the entity type as the connector does.

**coraine's own words**, read with `--fdscNgsild` only - the connector's PAP takes a word it does not map for
a literal, and a rule on it is never true there. Without `--fdscNgsild` a policy that uses one is not read,
and the log says why (`policy '<uid>' skipped: 'ngsi-ld:tenant' is coraine's own word - only with
--fdscNgsild`):

| word | reads |
|---|---|
| `ngsi-ld:tenant` | the request's `NGSILD-Tenant` - `""` for the default tenant; a notification: its subscription's tenant |
| `ngsi-ld:attribute` | **one attribute at a time**, expanded (the policy's names with the broker's default context, as `ngsi-ld:entityType`'s) |

`ngsi-ld:attribute`, decided attribute by attribute - an attribute is allowed when a policy allows it, not
necessarily the one that allows another:

| | |
|---|---|
| a read of entities - a retrieve, a query, a temporal read, a Snapshot's entities, a notification | the attributes no policy allows are **left out**; `id`, `type` and the entity's other members stay |
| `GET /entities/{id}/attrs/{attr}` | 403 for an attribute no policy allows |
| a write of attributes - a create, `PATCH`/`POST .../attrs`, a merge, `.../attrs/{attr}`, a batch entry, a temporal append | 403 (a batch: that entry's error) when one of them is not allowed |
| a write of the entity whole - a delete, a replace, a batch delete | needs a policy that holds whatever the attribute: one that holds for some attributes only is not enough |
| an operation not decided entity by entity (`/subscriptions`, `/types`) | a rule on `ngsi-ld:attribute` does not hold |

`auth_fdsc_ngsild_tenant` and `auth_fdsc_ngsild_attribute` hold them.

`auth_fdsc_ngsild_entity`, `auth_fdsc_ngsild_batch`, `auth_fdsc_ngsild_query`, `auth_fdsc_ngsild_temporal` and
`auth_fdsc_ngsild_notify` hold it; each of them fails without `--fdscNgsild`.

A credential kept as an owner is personal data at rest: it goes with its subscription (a DELETE of it), and
it is never checked again for revocation - VCVerifier checks that when it issues a token. `--fdscOwnerMaxAge`
bounds how long a revoked credential still receives notifications.

<a id="7-the-policy-api---the-broker-as-its-own-pap"></a>

## 7. The policy API - the broker as its own PAP

With **`--policyApiPort <port>`** the broker serves the ODRL-PAP's `/policy` API (odrl-pap 1.7.3,
`api/odrl.yaml`) on a port of its own, and `fdsc.so` reads the policies from it as it reads the PAP's:
`--fdscPap http://127.0.0.1:<port>`. The policies are documents of the default tenant's collection
`odrlPolicies`, in the broker's store: they outlive the broker. Built with `COR_FEATURE_POLICY_API` (on by default); off unless the port is given.

| | |
|---|---|
| `GET /policy?page&pageSize` | the policies in the order they were created - `[{"id", "odrl:uid", "odrl"}]`, `"odrl"` the policy as a string; `pageSize` 25 by default |
| `POST /policy` | a policy: 200, `Location:` its id (ten letters) |
| `PUT /policy/{id}` | creates or replaces policy `{id}`: 200, `Location: {id}` |
| `GET /policy/{id}`, `GET /policy/odrl/{uid}` | one policy, by id or by `odrl:uid` - 404 |
| `DELETE /policy/{id}`, `DELETE /policy/odrl/{uid}` | 204, whether there was one or not |

A policy is a JSON object with a string `odrl:uid`, one policy to a uid. `fdsc.so` reads it as it reads the
PAP's (§ 3.3): one it cannot read is skipped, with a log line.

**The port has no authentication**, as the PAP has none - in the connector the PAP is reached only from
inside the cluster. The auth plugin is not asked for its requests, and whoever reaches the port can change
every policy. `--policyApiHost` (default `127.0.0.1`) is the address it listens on: the default keeps it to
the machine; in Kubernetes, a Service of its own for the port, and a NetworkPolicy that lets in only what
writes policies (contract-management).

Each port serves its own routes: `/policy` is not on the NGSI-LD port, the NGSI-LD API is not on the policy
port. The policy port has no WebSocket upgrade, and it needs libmicrohttpd - a broker built with the
built-in HTTP server refuses `--policyApiPort` at startup.

**Where it answers otherwise than the PAP:**

| | PAP 1.7.3 | coraine |
|---|---|---|
| a body that is no JSON object, or has no `odrl:uid` | 500 | 400, a ProblemDetails |
| a left operand without its argument (`"odrl:leftOperand": "http:bodyValue"`) | 500, not stored | 400, the PAP's message, not stored |
| a `odrl:uid` another policy has | 500 | 409 |
| `PUT /policy/{id}` of a policy that exists | 500 (whatever the body) | replaced |
| a create's body | the Rego it made, `text/plain` | empty: there is no Rego |
| `"odrl"` | the policy rewritten - its `@context` replaced by the PAP's, an array of one made an object | the policy as it was given |
| `"rego"` | the Rego | not there |
| `/service/...`, `/template/...`, `/validate`, `/mappings`, the OPA bundles | served | not served |

contract-management (FDSC) reads only a create's status and deletes by `odrl:uid`
(`PAPAdapter.java`); the rest of the PAP's API is for its UI and for OPA.

`auth_fdsc_policy_api` holds it.

## 8. `oidc.so` - an OpenID provider's tokens and roles

`--auth oidc`: the access tokens of an OpenID provider, and two roles in them. Simpler than `fdsc.so` - no
policies, nothing decided per entity: each request is decided at its admission, on its method and its
NGSI-LD operation, before the broker fetches a `@context` for it. Built with `COR_FEATURE_AUTH_OIDC` (on;
off with `COR_FEATURE_AUTH` off). The broker is the same with it or without it: the flag decides whether
`oidc.so` is built.

**The token** - `Authorization: Bearer`, a JWT the provider signed with RS256 or ES256, checked against its
JWK Set; `iss` must be `--oidcIssuer`, `exp` and `nbf` hold within `--oidcLeeway`, `aud` holds
`--oidcAudience` when it is given. No token, or one that does not hold: **401**, with `WWW-Authenticate:
Bearer` (and `error="invalid_token"` for one that does not hold). A token is checked once; the requests that
send it again find it in a cache - only its `exp` and `nbf` are checked again.

**The keys** - from the provider's discovery document, `<issuer>/.well-known/openid-configuration`, or the
one `--oidcDiscovery` names; its `issuer` must be `--oidcIssuer` (OpenID Connect Discovery 1.0 § 4.3), its
`jwks_uri` gives the keys. `--oidcJwks` gives the JWK Set itself (a URL or a file) instead. Fetched by a
thread of the plugin, every `--oidcRefresh` seconds, every second until there are some, and at once (not
more than once a second) when a token names a key the set does not have - the provider rotated its keys.
Until the keys are there every request is a **503**.

**The roles** - in the claim `--oidcRolesClaim` names, a dot-separated path: an array of strings, or a
string of space-separated values (`scope`). Two:

| role | default name | may |
|---|---|---|
| read | `ngsild-read` (`--oidcReadRole`) | `GET`, `HEAD`, `OPTIONS`, and the queries that are POSTs: `POST /entityOperations/query`, `POST /temporal/entityOperations/query`, `POST /temporal/timepoint/entityOperations/query` |
| write | `ngsild-write` (`--oidcWriteRole`) | every request, the reads too |

A token without the role a request needs: **403**, its detail naming the role.

**Subscriptions** - creating or updating one is a write. Its **owner** - the roles and tenants of the token
that created it or last updated it - is kept with it (§ 2.1) and holds for `--oidcOwnerMaxAge` seconds
(default 86 400, a day). Each notification is sent only while the owner holds, may read, and may use the
subscription's tenant (`notify`); after that none are sent, until a request with a valid token updates the
subscription and makes its sender the owner. Roles or tenants taken away at the provider stop a
subscription's notifications within `--oidcOwnerMaxAge` - a token's own `exp` is not what it lasts (a token
lives minutes, a subscription months).

**The tenants** - in the claim `--oidcTenantsClaim` names (default `tenants`), an array of strings or a string
of space-separated values: the tenants the token may use. `default` is the default tenant (a request
without `NGSILD-Tenant`), `*` every tenant. A token without the claim may use the default tenant only. A
request in another tenant: **403** - decided before the tenant is resolved, so a create in a tenant the
token may not use makes no tenant. `--oidcAnyTenant` turns the check off.

### 8.1 Options

The options, as [Command-line options](options.md#plugin-oidc-loaded-by-auth-oidc) has them:

<!-- options of the oidc plugin: copied from doc/options.md by tools/pluginOptions.sh - edit them there -->

#### `--oidcIssuer`

`-oidcIssuer` · `CORAINE_OIDCISSUER` · String · default: no default - the broker does not start
· values: the provider's issuer URL · plugin: oidc

The provider's issuer: the `iss` every token must have, compared as a string. Without `--oidcDiscovery` or
`--oidcJwks` the keys are found through `<issuer>/.well-known/openid-configuration` (a trailing `/` of the
issuer is dropped first). Required - without it the broker stops at start:
`auth plugin 'oidc': no provider: --oidcIssuer (a token's iss is always checked)`.

A token of another issuer is a 401 with `error_description="wrong issuer"`. The issuer is what
`GET /.well-known/oauth-protected-resource` gives as `authorization_servers`.

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city

#### `--oidcDiscovery`

`-oidcDiscovery` · `CORAINE_OIDCDISCOVERY` · String · default: no default - `<--oidcIssuer>/.well-known/openid-configuration`
· values: a URL (`http://`, `https://`), or a file (a path, or `file://`) · plugin: oidc

The provider's OpenID discovery document, for a broker that reaches the provider by another name than the
issuer's (inside a cluster: `http://keycloak:8080/...` while the tokens carry the public name). Its
`jwks_uri` gives the keys, and its `issuer` must be `--oidcIssuer` (OpenID Connect Discovery 1.0 § 4.3): a
document of another issuer is not used, the log says so
(`its issuer is '...', not --oidcIssuer's '...'`), and with no keys every request is a 503. The `jwks_uri`
in it must be one the broker can reach.

Not with `--oidcJwks`: the broker stops at start with
`auth plugin 'oidc': --oidcDiscovery and --oidcJwks: one or the other`.

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city \
            --oidcDiscovery http://keycloak:8080/realms/city/.well-known/openid-configuration

#### `--oidcJwks`

`-oidcJwks` · `CORAINE_OIDCJWKS` · String · default: no default - the keys come from the discovery document
· values: a URL (`http://`, `https://`), or a file (a path, or `file://`) · plugin: oidc

The provider's JWK Set itself, instead of the discovery document: no discovery document is read.
`--oidcIssuer` is still required (the `iss` of a token is always checked). Not with `--oidcDiscovery`. A
JWK Set that cannot be read is in the log
(`oidc: the provider's keys (<where>): cannot open '<file>'`), and until it can every request is a 503
(`the provider's keys are not loaded yet`). A file is read again every `--oidcRefresh` seconds, as a URL is
fetched.

    coraine --database corDB --auth oidc --oidcIssuer https://idp.example.org/realms/city --oidcJwks /etc/coraine/jwks.json

#### `--oidcAudience`

`-oidcAudience` · `CORAINE_OIDCAUDIENCE` · String · default: no default - `aud` is not checked
· values: an audience (a client id) · plugin: oidc

An audience the token's `aud` must hold (a string, or an array that has it). A token without it is a 401
with `error_description="wrong audience"`. Without the option every token the provider issued for any of
its clients is taken, and the broker says so at start
(`oidc: --oidcAudience not set - a token issued for any client of the provider is taken`). Keycloak puts a
client into `aud` only with an audience mapper on the client the applications log in with
([Authorization](authorization.md#82-with-keycloak)).

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city --oidcAudience coraine

#### `--oidcLeeway`

`-oidcLeeway` · `CORAINE_OIDCLEEWAY` · Int · default: `60`
· values: 0-600 (seconds) · plugin: oidc

The clock skew allowed between the broker and the provider, in seconds: a token is taken until `exp` plus
the leeway, and from `nbf` minus it. A token outside it is a 401 with
`error_description="expired, or not yet valid"`. A token found in the token cache has its `exp` and `nbf`
checked again with the same leeway on every request. Out of range: the broker does not start
(`value 601 for option '--oidcLeeway' exceeds its upper limit. Allowed range: 0-600`).

    coraine --auth oidc --oidcIssuer https://idp.example.org/realms/city --oidcLeeway 0

#### `--oidcRefresh`

`-oidcRefresh` · `CORAINE_OIDCREFRESH` · Int · default: `300`
· values: 1-86400 (seconds) · plugin: oidc

Seconds between two fetches of the provider's keys (the discovery document, then its `jwks_uri`; or
`--oidcJwks`). While there are no keys the fetch is repeated every second, whatever the option says - the
provider may start after the broker. A token signed with a key the set does not have (the provider rotated
its keys) is a 401 and has the keys fetched at once, at most once a second. A new key set empties the token
cache.

    coraine --auth oidc --oidcIssuer https://idp.example.org/realms/city --oidcRefresh 60

#### `--oidcRolesClaim`

`-oidcRolesClaim` · `CORAINE_OIDCROLESCLAIM` · String · default: `realm_access.roles`
· values: a claim's path, dot-separated · plugin: oidc

Where in the token the roles are: a path of claim names separated by dots, each one an object member. The
claim is an array of strings, or a string of space-separated values (`scope`). The default is Keycloak's
realm roles; `resource_access.<client>.roles` is a Keycloak client's roles, `scope`, `roles` or `groups`
other providers'. A token whose claim does not hold `--oidcReadRole` or `--oidcWriteRole` may do nothing:
403 (`the token has neither role '...' nor '...' (claim '...')`). An empty value stops the broker at start
(`--oidcRolesClaim, --oidcReadRole, --oidcWriteRole, --oidcTenantsClaim: none may be empty`).

    coraine --auth oidc --oidcIssuer https://idp.example.org --oidcRolesClaim scope

#### `--oidcReadRole`

`-oidcReadRole` · `CORAINE_OIDCREADROLE` · String · default: `ngsild-read`
· values: a role name · plugin: oidc

The role that reads: `GET`, `HEAD`, `OPTIONS`, and the queries that are POSTs -
`POST /ngsi-ld/v1/entityOperations/query`, `POST /ngsi-ld/v1/temporal/entityOperations/query` and
`POST /ngsi-ld/v1/temporal/timepoint/entityOperations/query`. A token with only this role that writes gets a
403 (`a write needs the role 'ngsild-write' (claim 'realm_access.roles')`). Not empty (see
`--oidcRolesClaim`).

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city --oidcReadRole city-viewer

#### `--oidcWriteRole`

`-oidcWriteRole` · `CORAINE_OIDCWRITEROLE` · String · default: `ngsild-write`
· values: a role name · plugin: oidc

The role that reads and writes: every request. Creating or updating a subscription is a write. Not empty
(see `--oidcRolesClaim`).

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city --oidcWriteRole city-editor

#### `--oidcTenantsClaim`

`-oidcTenantsClaim` · `CORAINE_OIDCTENANTSCLAIM` · String · default: `tenants`
· values: a claim's path, dot-separated · plugin: oidc

Where in the token the tenants it may use are: a dot-separated path, as `--oidcRolesClaim`; an array of
strings, or a string of space-separated values. `default` is the default tenant (a request without
`NGSILD-Tenant`), `*` every tenant. A token without the claim may use the default tenant only; a token whose
claim names tenants may use those and no other - the default tenant only when `default` is among them. A
request in another tenant is a 403 (`the token may not use the tenant 't1' (claim 'tenants')`), decided
before the tenant is resolved, so a create in a tenant the token may not use makes no tenant. Not empty (see
`--oidcRolesClaim`). Not read with `--oidcAnyTenant`.

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city --oidcTenantsClaim org.tenants

#### `--oidcAnyTenant`

`-oidcAnyTenant` · `CORAINE_OIDCANYTENANT` · Bool · default: `false`
· values: a flag · plugin: oidc

No tenant check: every token may use every tenant, and `--oidcTenantsClaim` is not read. The roles are
checked as without it. `GET /.well-known/oauth-protected-resource` then gives `"coraine_tenants": null`.

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city --oidcAnyTenant

#### `--oidcAnonymousRead`

`-oidcAnonymousRead` · `CORAINE_OIDCANONYMOUSREAD` · Bool · default: `false`
· values: a flag · plugin: oidc

A read without a token (the reads of `--oidcReadRole`) is let in, in every tenant. A write without a token
is still a 401 (`no access token (Authorization: Bearer)`). A request that does send a token is decided on
the token, as without the option: a token that does not hold is a 401 even for a read.

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city --oidcAnonymousRead

#### `--oidcOwnerMaxAge`

`-oidcOwnerMaxAge` · `CORAINE_OIDCOWNERMAXAGE` · Int · default: `86400` (a day)
· values: 1-31536000 (seconds) · plugin: oidc

A subscription's owner - the roles and tenants of the token that created it or last updated it - is kept
with the subscription and holds for this many seconds from that request. Each notification is sent only
while the owner holds, has a role that reads, and may use the subscription's tenant; after that none are
sent, until a request with a valid token updates the subscription and makes its sender the owner. This is
what bounds how long roles or tenants taken away at the provider still receive notifications: the token's
own `exp` is not used (a token lives minutes, a subscription months).

    coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city --oidcOwnerMaxAge 3600

<!-- end of the options of the oidc plugin -->

### 8.2 With Keycloak

```
coraine --auth oidc --oidcIssuer https://keycloak.example.org/realms/city --oidcAudience coraine
```

- realm roles `ngsild-read` and `ngsild-write`, given to the users and the service accounts that need them;
- the tenants: a user attribute `tenants` (several values: `default`, `t1`, ...), and on the client a "User
  Attribute" mapper - user attribute `tenants`, token claim name `tenants`, multivalued, added to the access
  token. Keycloak 26 keeps only the attributes its user profile declares: declare `tenants` there, or let the
  realm keep unmanaged attributes;
- an audience mapper on the client the applications log in with, adding `coraine` to the access token's
  `aud` - without one, Keycloak's access tokens do not carry it;
- a broker that reaches Keycloak by an internal name (`http://keycloak:8080`) while the tokens carry the
  public one adds `--oidcDiscovery http://keycloak:8080/realms/city/.well-known/openid-configuration`; the
  `jwks_uri` in it must be one the broker can reach.

## 9. Next

- A request a policy may allow - with `--fdscNgsild`, one whose decision depends on the entity (§ 6) - has
  its `@context` fetched before `entity()` decides it: a client with a right to some entity can have the
  broker fetch a URL of its choice.

## Sources (2026-10-12)

- `FIWARE/data-space-connector` 10.87.0: `k3s/provider.yaml`, `charts/data-space-connector/Chart.yaml`,
  `it/src/test/resources/policies/*.json`, `doc/deployment-integration/local-deployment/LOCAL.MD`
- `FIWARE/odrl-authorization` 2.4.3: `charts/odrl-authorization` (`files/apixis-opa.lua`, `files/apixis-helper.lua`,
  `templates/opa-cm.yaml`, `templates/routes.yaml`)
- `SEAMWARE/odrl-pap` 1.7.3: `src/main/resources/mapping.json`, `rego/ngsi-ld/*.rego`, `rego/vc/*.rego`,
  `rego/odrl/*.rego`, `rego/utils/apisix.rego`, `mapping/OdrlMapper.java`, `mapping/MappingResult.java`,
  `BundleResource.java`, `api/odrl.yaml`
- `FIWARE/VCVerifier` 6.23.1: `verifier/verifier.go`, `openapi/routers.go`, `openapi/api_api.go`, `config/config.go`
- `apache/apisix` 3.17.0 `apisix/plugins/openid-connect.lua`; `zmartzone/lua-resty-openidc` 1.8.0
