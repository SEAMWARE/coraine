# Contributing to coraine

Thanks for looking at coraine. These guidelines exist to make it obvious where help
is useful and under what terms contributions are accepted.

## Ground rules & expectations

- Be kind and thoughtful. People come from different backgrounds and carry different
  ideas of "how open source is done" — listen before convincing.
- This project is released with a [Contributor Code of Conduct](./CODE_OF_CONDUCT.md).
  By participating, you agree to abide by its terms.
- If you open a pull request, you must sign the
  [Individual Contributor License Agreement](https://fiware.github.io/contribution-requirements/individual-cla.pdf)
  by stating in a comment: *"I have read the CLA Document and I hereby sign the CLA"*.
- Contributions are accepted under the [Apache License 2.0](./LICENSE), the licence
  this project is distributed under. Every source file carries an SPDX identifier;
  keep it.
- A contribution must pass the whole functional-test suite. A change in behaviour
  comes with the test that pins it — see below.

## Intellectual property

By contributing, you certify that you wrote the contribution or otherwise have the
right to submit it under the Apache License 2.0, and you grant that licence to the
project. The signed CLA above is what records this. Nothing is merged without it.

## How to contribute

Search the [issues](https://github.com/SEAMWARE/coraine/issues) and
[pull requests](https://github.com/SEAMWARE/coraine/pulls) first — someone may have
raised the same idea.

- **Minor contribution** (a bug fix): open a pull request.
- **Major contribution** (a new feature, a change of behaviour): open an issue first,
  so the design can be discussed before anyone writes code.

Pull requests go against `master`. Keep a pull request to one subject; a commit
message says *why*, not only *what*.

## Building and testing

Building the broker and running the suite is described in the
[README](./README.md) — the dependency stack, the `makefile` targets, and how the
functional tests run through `corTest`.

Two rules matter more than the rest:

- **A behaviour change carries its test.** A fix without a test that fails before it
  and passes after it is not finished.
- **The suite must be green before a pull request.** `make test` runs it against
  MongoDB; `corTest -db ramdb` runs the in-memory variant. Both are expected to pass.

Coverage of the suite is measured per DB and reported in
[`doc/coverage.md`](./doc/coverage.md).

## Where the work is

[`ToDo.md`](./ToDo.md) is the backlog and the public roadmap: what is not built yet,
what is deferred by design, and why. It is the honest answer to "what needs doing".

## Reporting a bug

Open a [GitHub issue](https://github.com/SEAMWARE/coraine/issues) with the broker
version (`GET /admin/version`, with the `admin` API plugin loaded), the request that triggered it, the
response you got and the one you expected. A failing functest is the most useful bug
report there is.
