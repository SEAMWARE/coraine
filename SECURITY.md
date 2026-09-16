# Security policy

## Reporting a vulnerability

**Please do not open a public issue for a security problem.**

Use GitHub's private vulnerability reporting, which is enabled on this
repository:

> [**Report a vulnerability**](https://github.com/SEAMWARE/coraine/security/advisories/new)

That opens a private advisory visible only to the maintainers. It is the
preferred route because the report, the discussion, the fix and the eventual
disclosure all stay in one place, and a CVE can be requested from it.

If you cannot use GitHub advisories, open a regular issue saying only that you
have a security report and asking for a contact — no details — and a maintainer
will reply with a private channel.

### What to include

Whatever you have. These help most:

- the version, or the commit — `GET /ngsi-ld/v1/version` reports both, and
  `GET /ngsi-ld/v1/build` reports the compile-time feature set
- the configuration: which `--database` and `--troe` plugins, `COR_HTTP_SERVER`,
  and whether the broker was reachable from outside the host
- a request that triggers it, ideally as `curl`
- what you expected and what happened

A crash, a hang, a memory error, an information leak across tenants, or a way
past a restriction the API states are all worth reporting even if you are not
sure they are exploitable. So is anything that makes the broker answer for data
belonging to another tenant.

### What to expect

- an acknowledgement within a few working days
- an assessment of whether we agree it is a vulnerability, with reasoning
- a fix on a private branch, with a functional test that fails without it
- coordinated disclosure: the advisory is published together with the release
  that fixes it, and you are credited unless you ask not to be

There is no bounty programme.

## Supported versions

coraine is pre-1.0 and moves fast. Fixes land on `main` and in the next release;
older releases are not patched. Run a recent release, or `main`.

| Version | Supported |
|---------|-----------|
| `main` | ✅ |
| latest release | ✅ |
| anything older | ❌ — upgrade |

## Deployment notes that are not vulnerabilities

Worth stating, because each of these has been reported as a bug against other
brokers:

- **The broker serves HTTP, not HTTPS**, when built with the internal HTTP
  server (`COR_HTTP_SERVER=builtin`). TLS termination belongs to a proxy in
  front of it. The libmicrohttpd build can serve HTTPS directly.
- **There is no authentication or authorisation in the broker.** NGSI-LD does
  not define any, and coraine implements the specification. Access control
  belongs to an API gateway — in FIWARE, to an identity manager and a PEP proxy
  in front of the broker. A broker exposed directly to an untrusted network is
  readable and writable by that network, by design.
- **Tenants are a namespace, not a security boundary.** `NGSILD-Tenant`
  separates data; it does not authenticate the claim to a tenant. A leak
  *between* tenants without the header being supplied is a bug — and a serious
  one. Honouring the header a client sends is not.
