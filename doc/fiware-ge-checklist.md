# FIWARE Generic Enabler — compliance checklist

coraine is a candidate FIWARE Generic Enabler in the **Core Context Management**
chapter. This file tracks the requirements from
[FIWARE/contribution-requirements](https://github.com/FIWARE/contribution-requirements)
against the state of this repository, so the gap is visible rather than remembered.

Status: ✅ done · ⏳ in progress or blocked · ⛔ needs an account, a decision or the
FIWARE Foundation.

## MUST

| Requirement | State | Note |
|-------------|-------|------|
| Code repository on GitHub | ✅ | `github.com/SEAMWARE/coraine` |
| Documentation source on GitHub | ✅ | `doc/` |
| Open-source licence | ✅ | Apache 2.0, SPDX header in every source file |
| Default branch is `master` or `develop` | ✅ | `main` — the org’s public repos all use it; FIWARE accepts either |
| README — description of the GE | ✅ | |
| README — table of contents | ✅ | |
| README — FIWARE paragraph | ✅ | chapter and catalogue named |
| README — badges (chapter, licence, docs, container, status) | ⏳ | present and resolving; the **OpenSSF badge is absent** until the project is registered — a placeholder id renders a 404 |
| README — CI build badge | ⛔ | no CI yet — see below |
| README — how to deploy | ✅ | Building / Running, plus `docker/README.md` |
| README — API walkthrough | ✅ | summary in README, full version in `doc/api-walkthrough.md` |
| README — access to advanced API and documentation | ✅ | Documentation section |
| README — QA section | ✅ | ratings badges pending the Catalogue entry |
| README — training linkbox | ✅ | |
| CONTRIBUTING with IPR terms, linked from README | ✅ | `CONTRIBUTING.md`, CLA named |
| CREDITS | ✅ | `CREDITS.md` |
| Public roadmap | ✅ | `doc/roadmap.md`, detail in `ToDo.md` |
| Docs — installation / administration guide | ✅ | `doc/installation.md` |
| Docs — user / programmer guide | ✅ | `doc/api-walkthrough.md` + the spec |
| Docs — every config element documented | ✅ | full option table; `coraine --usage` is generated from the same table |
| Docs — every environment variable documented | ✅ | the `CORAINE_<OPTION>` rule, printed per option by `coraine -U`, plus `SEAMWARE_PLUGIN_DIR` |
| Docs — no `<a>` anchor tags | ✅ | pure Markdown |
| Docs on Read the Docs, Markdown | ⏳ | `mkdocs.yml` + `.readthedocs.yml` in place; **the RTD project must be created** |
| Docs — chapter CSS | ✅ | `fiware_readthedocs.css` in `mkdocs.yml` |
| Docs — chapter badge on the docs home page | ✅ | `doc/index.md` |
| Docs — analytics configured | ⛔ | needs a Google Analytics property shared with `fiware.eu@gmail.com` |
| Docker — Dockerfile present | ✅ | `docker/Dockerfile` |
| Docker — README present | ✅ | `docker/README.md` |
| Docker — image available, SemVer tag | ⏳ | `quay.io/coraine/coraine`, published by deploy.yml on merge to main (moving to `seamware` when the org allows) |
| Docker — no fixed ports | ✅ | `--port`, nothing hard-wired in the image |
| Releases on GitHub, SemVer | ⏳ | version string is SemVer; **no release tagged yet** |
| No stale PRs older than 90 days | ✅ | |
| Entry in the FIWARE Catalogue | ⛔ | Foundation |
| Signed Contributor License Agreement | ⛔ | Foundation |
| TSC candidature, accepted into a chapter | ⛔ | Foundation |
| Attend roadmap meetings | ⛔ | people, not files |
| OpenSSF Best Practices badge passing (Full membership) | ⛔ | register the project, then fill the questionnaire |

## SHOULD

| Requirement | State | Note |
|-------------|-------|------|
| Standard-README structure | ✅ | |
| GitHub repository description not blank | ⏳ | set when the repository goes public |
| GitHub mandatory topics | ⏳ | same |
| Stack Overflow tag set up | ⛔ | the `coraine` tag has to be created by asking a question with it |
| CI running on pull requests, with unit tests | ⛔ | **blocked**: organisation secrets do not reach private repositories on the Free plan, so CI cannot run until the repositories are public |
| Release aligned with the FIWARE release schedule | ⛔ | once in the Catalogue |
| Configurable through environment variables | ✅ | every option has a `CORAINE_<OPTION>` variable |
| Linter / automated code format | ⏳ | style is enforced by review; no linter wired in |
| Tutorial provided | ⏳ | `doc/api-walkthrough.md` is the short one |

## The two real blockers

1. **The repositories are private.** CI, the container publishing that depends on it,
   the CI badge and the repository metadata all wait on that single decision.
2. **The FIWARE Foundation side** — TSC candidature, the CLA, the Catalogue entry and
   the OpenSSF registration are applications, not commits.
