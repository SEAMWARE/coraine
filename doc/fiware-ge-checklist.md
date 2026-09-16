# FIWARE Generic Enabler — compliance checklist

coraine is a candidate FIWARE Generic Enabler in the **Core Context Management**
chapter. This file tracks the requirements from
[FIWARE/contribution-requirements](https://github.com/FIWARE/contribution-requirements)
against the state of this repository, so the gap is visible rather than remembered.

It is organised the way the Foundation's own checklist is — **New GE** (what is
needed to be accepted as Incubated), then **Incubated** (expected within months
of acceptance), then **Mature** (full membership) — because those are three
different bars and conflating them makes the near one look further away than it
is.

> **Incubation does not require full compliance.** The Foundation's words:
> *"It is accepted that Incubated Enablers may not currently comply with all
> 'MUST' requirements of the Development Guidelines, but are expected to be
> working towards fulfilling them."* What is required at this stage is the New
> GE list below plus a credible trajectory.

Status: ✅ done · ⚠️ a real gap we can close ourselves · ⛔ needs the FIWARE
Foundation, an account, or a decision that is not a commit.

Last verified against the repository: **2026-09-16**.

## New Generic Enabler — the bar for acceptance as Incubated

| Requirement | State | Note |
|-------------|-------|------|
| Presented to the TSC for candidature, accepted into a Chapter | ⛔ | The chapter is **Core Context Management**; the badge is already on the README |
| Show how it integrates within the overall FIWARE Architecture | ✅ | [Architecture fit](#architecture-fit) below — north/southbound, federation via registrations, and which pieces are optional, with a diagram. ⚠️ ASCII, so it renders anywhere; the TSC deck wants it drawn properly |
| Signed harmonized [Entity CLA](https://fiware.github.io/contribution-requirements/entity-cla.pdf) | ⛔ | A PDF to sign, not a commit |
| Codebase available on GitHub | ✅ | `github.com/SEAMWARE/coraine`, public |
| Appropriate open-source licence | ✅ | Apache 2.0 — `LICENSE` at the root, detected by GitHub as `Apache-2.0`, SPDX header in every source file |
| **Instantiable directly in a running state by any competent developer, without referring to other dependent technologies** | ✅ | `coraine --database corDB` — one process, 4.3 MiB of added files, no database server, ready ~12 ms after `exec`. See the note below |
| Dockerfile available within the codebase | ✅ | `docker/Dockerfile`, plus `docker-compose.yml` and `docker-compose-distops.yml` |
| Basic documentation in Markdown, complete enough for usage | ✅ | all of `doc/`, published on Read the Docs |
| — a Quick Start guide | ✅ | README § Quick start |
| — Admin installation: standalone with configuration files | ✅ | [`doc/installation.md`](installation.md) |
| — Admin installation: Docker / docker-compose with `ENV` variables | ✅ | same, and every CLI option has a `CORAINE_<OPTION>` environment variable |
| — Developer documentation: how to call the API programmatically | ✅ | [`doc/api-walkthrough.md`](api-walkthrough.md) |
| — User documentation: how to use the GUI | n/a | there is no GUI |
| — Apiary / API specification | ✅ | Referenced rather than restated — README § API specification links ETSI ISG CIM's OpenAPI 3.0.3 on ETSI Forge (publicly readable, no account) and names ETSI GS CIM 047. ⚠️ Stated honestly there: that file was last updated **April 2022** and describes ~v1.7.1–1.8.1, not the v1.9.1 implemented here; an ETSI STF is producing a current one. For the newer clauses the specification document is the authority |
| **Stateful component: state persistable between instantiations, no manual set-up** | ⚠️ | True with `--database mongoc`. **Not** true with `corDB`, which is in-process and does not persist yet — see the note below. Persistence is [ToDo § 15](https://github.com/SEAMWARE/coraine/blob/main/ToDo.md) |
| Commitment to complete the remaining Incubated requirements | ✅ | this file is the tracking |

### Two notes that pull in opposite directions

**The zero-dependency requirement is where coraine is strongest.** Every example
the Foundation gives for "instantiable without referring to dependent
technologies" is about *hiding* a dependency — Keyrock behind MySQL, an IoT
Agent behind MongoDB, QuantumLeap behind CrateDB. coraine with `corDB` has no
dependency to hide: no database server, no runtime, no JVM, three added shared
libraries of which two are GEOS.

**Persistence is where it is weakest, and it is the same feature.** A `corDB`
deployment does not survive a restart. The requirement is met by running
`--database mongoc`, which is a supported, tested configuration — so the honest
answer is "persistent via MongoDB today, and `corDB` persistence is on the
roadmap", not "we need no database". Claiming the second while the checklist
asks the first is how a reviewer loses trust in the rest of the answers.

### Submission checklist

Sent to FF Staff with the [application form](https://www.fiware.org/catalogue/submit-your-software/):

| Item | State | Value |
|------|-------|-------|
| URLs of all GitHub repositories holding the code base | ✅ | `SEAMWARE/coraine` plus the libraries it builds on: `corRest`, `corHttp`, `corNgsild`, `corJsonld`, `corPlugin`, `corLibs`, `corTest`, and the k-libs on GitLab |
| Version number of the initial catalogue release, matching a real tag | ✅ | **0.4.0** — tag `v0.4.0`, GitHub release published 2026-08-28 |
| Location of the official `Dockerfile` | ✅ | `docker/Dockerfile` |
| URL of the official documentation, ideally Read the Docs | ✅ | `https://coraine.readthedocs.io` |
| Name and email of the owner/caretaker | ✅ | Ken Zangelin |
| Elevator pitch, "What is?" and "Why use?" texts | ✅ | drafted below — to be pasted into the form, and worth moving into the README |

## Submission texts

**Elevator pitch** (one sentence):

> coraine is an NGSI-LD context broker written in C that runs as a single
> 4 MiB process with no database server, and still implements the whole of
> ETSI GS CIM 009 v1.9.1.

**What is coraine?** (one paragraph):

> coraine is a FIWARE Generic Enabler in the Core Context Management chapter: an
> NGSI-LD context broker implementing ETSI GS CIM 009 v1.9.1 — entities,
> subscriptions, registrations, distributed operations, temporal queries and
> GeoJSON — and passing the ETSI conformance test suite. It is written in C and
> built as a plugin host: the current-state store, the temporal backend and
> additional API surfaces are shared libraries loaded at start-up. Deployed with
> its in-process store it is one executable and three shared libraries, 4.3 MiB
> of files a machine did not already have, answering requests 12 ms after it is
> started. Deployed with MongoDB and TimescaleDB it is the same broker with
> persistence and history, configured by a command-line flag.

**Why use coraine?** (one paragraph):

> Because the broker stops being the part of the deployment you have to plan
> around. A conventional NGSI-LD deployment is a broker plus a MongoDB server
> plus a time-series database; on eight shared cores, one broker core doing
> batch writes through MongoDB needs four MongoDB cores behind it before the
> database stops being the bottleneck. coraine's in-process store removes both
> servers: the same eight cores serve 40 000 queries a second instead of 23 000,
> temporal history costs nothing measurable instead of 91% of the write rate,
> and the installed footprint drops from 24 third-party libraries to three. That
> makes it deployable where a broker did not previously fit — a gateway, an
> edge node, a container with one core — while remaining the same
> specification-complete broker in the data centre. Every number here is
> measured and reproducible: see [Performance and footprint](performance.md).

## Architecture fit

Where coraine sits in the FIWARE architecture, for the "MUST show how it
integrates" requirement:

- **Chapter: Core Context Management.** coraine is a context broker — the same
  role and the same NGSI-LD interface as the other brokers in the chapter, so
  anything that speaks NGSI-LD to a FIWARE broker speaks it to coraine
  unchanged. That includes the FIWARE tutorials, which now offer coraine as a
  broker option.
- **Northbound**, it serves the NGSI-LD API over HTTP(S) to applications and to
  other FIWARE components — Wirecloud, Knowage, Cygnus, Draco, QuantumLeap —
  and notifies subscribers over HTTP or MQTT.
- **Southbound**, it accepts context from IoT Agents exactly as any NGSI-LD
  broker does. It is also able to *be* the agent: the same source code compiles
  to a reduced "cor-agent" configuration, so a small deployment need not run
  both.
- **Context Source Registrations** make it a federation member: coraine forwards
  and aggregates across registered sources, and can itself be registered as a
  source in someone else's federation.
- **Temporal history** goes to PostgreSQL/TimescaleDB through the TRoE plugin,
  or stays in the process — so the historical-data chapter is an option rather
  than a prerequisite.

```text
                    applications · Wirecloud · Knowage · dashboards
                                        │
                                        │  NGSI-LD  (HTTP/HTTPS)
                                        ▼
  ┌──────────────────────────────────────────────────────────────────────────┐
  │                      c o r a i n e   (Core Context Management)           │
  │                                                                          │
  │   NGSI-LD API  ·  entities · subscriptions · registrations · temporal    │
  │                   geo-queries · distributed operations                   │
  │                                                                          │
  │   ┌──────────────────┐   ┌──────────────────┐   ┌────────────────────┐   │
  │   │ current state    │   │ temporal (TRoE)  │   │ API surfaces       │   │
  │   │ ── plugin ──     │   │ ── plugin ──     │   │ ── plugins ──      │   │
  │   │ corDB │ mongoc   │   │ corDB │ timescale│   │ admin · metrics    │   │
  │   └────────┬─────────┘   └────────┬─────────┘   └────────────────────┘   │
  └────────────┼──────────────────────┼───────────────────────────────────────┘
       in this │ process              │ in this process
               │   or                │    or
               ▼                      ▼                     │            ▲
          MongoDB                PostgreSQL +               │            │
          (optional)             TimescaleDB                │            │
                                 (optional)                 │            │
                                                notifications│            │ context
                                                 HTTP · MQTT │            │
                                                             ▼            │
                                              subscribers          IoT Agents,
                                                                   devices, or
                                                                   coraine itself
                                                                   as "cor-agent"
                          ┌───────────────────────────────────────────┐
                          │  federation, via Context Source           │
                          │  Registrations: coraine forwards to and   │
                          │  aggregates from other NGSI-LD sources,   │
                          │  and can be registered as one itself      │
                          └───────────────────────────────────────────┘
```

Three things the diagram is meant to make obvious:

- **The dashed boxes are plugins, and two of them can be nothing at all.** With
  `--database corDB --troe corDB` the two optional servers below the line
  disappear and the broker is one process. That is the same broker, the same
  API, and the same conformance results — configured by a flag, not a different
  product.
- **The southbound arrow points both ways.** coraine accepts context from IoT
  Agents like any NGSI-LD broker, and the same source code compiles to a
  reduced agent configuration, so a small deployment need not run both.
- **Federation is not a separate component.** Registrations make any coraine a
  member of a federation of NGSI-LD sources, in either direction.

⚠️ This is the text-and-ASCII version, which renders anywhere. For the TSC
presentation it wants drawing properly.

## Incubated Generic Enabler — expected within months of acceptance

| Requirement | State | Note |
|-------------|-------|------|
| Entry in the FIWARE Catalogue under the agreed Chapter | ⛔ | Foundation |
| Mirroring GitHub webhook configured | ⛔ | Foundation configures it |
| Read the Docs project available | ✅ | `coraine.readthedocs.io`, building on every merge |
| Read the Docs styled with the FIWARE and Chapter CSS | ✅ | `fiware_readthedocs.css` in `mkdocs.yml`; chapter badge on `doc/index.md` |
| Automatic documentation generation configured | ✅ | `mkdocs.yml` + `.readthedocs.yaml`, with `fail_on_warning: true` so a dead cross-reference fails the build instead of shipping |
| Analytics configured for the Read the Docs portal | ⚠️ | Not configured. Needs a Google Analytics property shared with the Foundation |
| Docker image available in **FIWARE's** Docker Hub account | ⚠️ | Images are published to `quay.io/seamware/coraine` on every merge to main, one immutable tag per merge, never `latest`. The FIWARE-account image is a Foundation step |
| Roadmap available and linked from the FIWARE roadmap | ✅ | [`doc/roadmap.md`](roadmap.md), with the detail in `ToDo.md` — the link into the FIWARE wiki is a Foundation step |
| Release schedule aligned with the FIWARE release schedule | ⛔ | once in the Catalogue |
| Access provided for the FIWARE monitoring tool | ⛔ | Foundation |

## Mature Generic Enabler — full membership

| Requirement | State | Note |
|-------------|-------|------|
| All FIWARE **MUST** requirements fulfilled | ⚠️ | the gaps above, plus the two process requirements below |
| OpenSSF Best Practices badge displayed **and passing** | ⚠️ | Not registered — that needs an account. The repository side is now in place: `SECURITY.md` with a vulnerability-reporting process, GitHub private vulnerability reporting **enabled**, secret scanning **enabled**, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `CHANGELOG.md`, issue and pull-request templates, tests run automatically, and a documented release process |
| Demonstrated real need, with adoption statistics presented to the TSC | ⛔ | needs users, not commits |
| Stack Overflow tag registered | ⛔ | creating a tag needs reputation the maintainers do not have; support is documented as GitHub issues |
| Public instance on FIWARE infrastructure (where suitable) | ⛔ | Foundation, and optional |
| Tour guide entry | ⛔ | Foundation |

### Two development-lifecycle requirements that need a decision

Relaxed during incubation, required to graduate, and both are structural rather
than technical:

- **"Pull Request approval MUST be issued by a third party. Nobody can
  auto-merge their contributions."** Worth asking the TSC how they want this
  evidenced rather than assuming it is a barrier. Every change goes through a
  pull request, and the person who writes the change is not the person who
  approves and merges it: the implementation is done by an agent working in the
  open, the maintainer reviews the diff and the test results and merges. The
  intent of the rule — that nobody lands their own work unreviewed — is met.
  What the rule cannot be read to require, on the evidence of how it has been
  applied to mature GEs in this chapter, is two salaried humans; that has not
  been the standard in practice.
- **"Before merging an open Pull Request CI status MUST be green."** CI runs on
  every pull request (functests against both `corDB` and `mongoc`, sharded by
  time) and a nightly adds valgrind, the ETSI conformance suite, coverage and
  the performance run. The requirement is about discipline, not capability:
  PR #85 was merged with the Read the Docs check red. It was a broken
  cross-reference and harmless, but it is exactly what FIWARE QA inspects.

## Other general requirements

| Requirement | State | Note |
|-------------|-------|------|
| GitHub Issues used for issue tracking | ✅ | enabled and in use |
| README — description, table of contents, install, usage, API, licence | ✅ | |
| README — badges: chapter, licence, container, release, docs, support | ✅ | six badges, all resolving |
| README — CI build badge | ✅ | two, in fact: the per-PR CI workflow and the nightly |
| Documentation in Markdown | ✅ | no reStructuredText, no `<a>` anchor tags |
| Repository description not blank | ✅ | set |
| Mandatory GitHub topics | ✅ | `c`, `context-broker`, `etsi`, `fiware`, `iot`, `linked-data`, `ngsi-ld` |
| Configurable through environment variables | ✅ | `CORAINE_<OPTION>` for every option |
| No fixed ports in the image | ✅ | `--port`, nothing hard-wired |
| No stale pull requests older than 90 days | ✅ | none open |
| Security policy / vulnerability reporting | ✅ | `SECURITY.md`, and GitHub private vulnerability reporting is enabled — plus what is *not* a vulnerability (no auth in the broker, tenants are a namespace) so reports arrive about real things |
| Contributing guide, code of conduct, changelog | ✅ | `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md` (Contributor Covenant), `CHANGELOG.md` |
| Issue and pull-request templates | ✅ | `.github/ISSUE_TEMPLATE`, `.github/PULL_REQUEST_TEMPLATE.md` — the PR template asks for the evidence that a fix's test fails without the fix |
| Repository homepage set | ✅ | `coraine.readthedocs.io` |
| Secret scanning | ✅ | enabled (detection). Push protection deliberately left off: the test fixtures contain token-shaped strings and a false positive that blocks a push costs more than it saves |
| Tests, run automatically | ✅ | ~600 functional tests per database, ETSI conformance suite, valgrind, coverage |
| Linter / automated code format | ⚠️ | Style is enforced by review; nothing wired in. Deliberate for now — a formatter imposed on this codebase would rewrite comment alignment that carries meaning, so it needs a configuration written for it rather than a default |
| Tutorial provided | ✅ | [`doc/api-walkthrough.md`](api-walkthrough.md), and coraine is now a broker option in the FIWARE tutorials |

## What is ours to close, shortest first

1. **Read the Docs analytics** — needs a Google Analytics property, created under
   an account and shared with the Foundation. Nothing in the repository blocks it.
2. **OpenSSF Best Practices registration** — needs a login. The repository side is
   done, so this is filling in a questionnaire whose answers are already true.
3. **The architecture diagram, drawn** — the ASCII one above is the substance;
   the TSC deck deserves a real figure.
4. **corDB persistence** — the only *code* left on this list, and the one that
   matters most: see below.
5. **How third-party approval should be evidenced** — a question for the TSC, not
   a code change.

## The one that is not paperwork

Everything above is a form, a badge or a figure. One item is engineering, and it
is the item a reviewer will press on:

> For a stateful component, the state **MUST** be persistable between
> instantiations, additional manual set-up **MUST** not be required.

`--database mongoc` satisfies this today, so the requirement is met and the
checklist is honest. But the configuration coraine is *interesting* for — one
process, no servers, 4.3 MiB — is the one that loses its data on restart, and
that is the configuration the zero-dependency requirement rewards. Until corDB
persists, the two requirements pull in opposite directions and we have to pick
which one to lead with.

Design and staging: [ToDo § 15](https://github.com/SEAMWARE/coraine/blob/main/ToDo.md).

Everything else on the ⛔ lines is an application to the FIWARE Foundation or a
consequence of one.
