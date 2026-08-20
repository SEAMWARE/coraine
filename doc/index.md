# coraine

A lightweight **NGSI-LD Context Broker** written in C, fully implementing
**ETSI GS CIM 009 v1.9.1** and passing the official ETSI NGSI-LD conformance test
suite.<sup>\*</sup>

coraine is small — a 967 KiB stripped binary that starts in 10 milliseconds — and
plugin-driven: the storage backend, the temporal history, the extra API surfaces and
(next) the wire protocol itself are shared libraries chosen at startup. The core
broker speaks NGSI-LD; the plugins decide where data lives, what extra endpoints
exist, and how the broker talks to the world.

<sup>\*</sup> The conformance runs use a corrected fork of the ETSI suite. The changes
are test-side fixes — the suite has bugs of its own and parts of it do not run as
published — never relaxations of what the broker must do. They are filed upstream.

## Where to go

| If you want to | Read |
|----------------|------|
| install, build and run it | [Installation & Administration](installation.md) |
| see the API in action | [API walkthrough](api-walkthrough.md) |
| understand how it is put together, or write a plugin | [Plugin architecture](plugin-architecture.md) |
| judge how well it is tested | [Test coverage](coverage.md) |
| know what is not built yet | [Roadmap](roadmap.md) |

## Support

Questions are answered on
[Stack Overflow](https://stackoverflow.com/questions/tagged/fiware-coraine) under the
`fiware-coraine` tag. Bugs and feature requests belong in
[GitHub issues](https://github.com/SEAMWARE/coraine/issues).

## License

[Apache License 2.0](https://github.com/SEAMWARE/coraine/blob/master/LICENSE) —
Copyright 2026 Seamware.
