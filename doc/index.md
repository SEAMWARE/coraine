# coraine

A lightweight **NGSI-LD Context Broker** written in C, fully implementing
**ETSI GS CIM 009 v1.9.1** and passing the official ETSI NGSI-LD conformance test
suite.<sup>\*</sup>

coraine is small — one process, 18 MiB of RAM, answering its first request 13 ms
after `exec`, with no other service required — and plugin-driven: the storage
backend, the temporal history, the extra API surfaces and (next) the wire
protocol itself are shared libraries chosen at startup. The core
broker speaks NGSI-LD; the plugins decide where data lives, what extra endpoints
exist, and how the broker talks to the world.

<sup>\*</sup> The conformance runs use a corrected fork of the ETSI suite. The changes
are test-side fixes — the suite has bugs of its own and parts of it do not run as
published — never relaxations of what the broker must do. They are filed upstream,
as merge requests for the suite's maintainers to review; until they are merged, the
result is against the corrected suite, not the published one: 1106 of 1106 test
cases, on MongoDB + TimescaleDB and on `corDB` (2026-10-07).

## Where to go

| If you want to | Read |
|----------------|------|
| install, build and run it | [Installation & Administration](installation.md) |
| run several instances behind a load balancer | [High Availability](high-availability.md) |
| see the API in action | [API walkthrough](api-walkthrough.md) |
| understand how it is put together, or write a plugin | [Plugin architecture](plugin-architecture.md) |
| know what it costs to run, and how fast it is | [Performance and footprint](performance.md) |
| build a broker for your own workload - the profile, the allocator, the lock | [Extreme performance](extreme-performance.md) |
| judge how well it is tested | [Test coverage](coverage.md) |
| know what is not built yet | [Roadmap](roadmap.md) |
| see every idea, planned or only possible | [Ideas](ideas.md) |
| connect it to DDS or ROS 2 | [DDS and ROS 2](dds.md) |
| see how it will reach devices without an IoT Agent | [Speaking to devices directly](device-protocols.md) |
| read Modbus devices and PLCs, and write to them | [The Modbus bridge](modbus-bridge.md) |
| send notifications over MQTT, or map MQTT topics to attributes | [The MQTT bridge](mqtt-bridge.md) |
| see how authorization will work inside the broker | [Authorization in the broker](authorization.md) |
| see the binary format for snapshots, the log and cor:// (design draft) | [The cor format and cor://](cor-protocol.md) |
| use the API over a WebSocket and get the notifications on it | [WebSocket](websocket.md) |
| see where a request runs - inline, as a coroutine, on a worker - and how it waits | [Coroutines](coroutines.md) |
| read how it got here - designs dropped, regressions found, before and after | the History pages, e.g. [coroutines](history/coroutines.md), [performance](history/performance.md) |

## Support

Questions, bugs and feature requests all belong in
[GitHub issues](https://github.com/SEAMWARE/coraine/issues) — that is where the
maintainers are. General FIWARE questions also reach people under the
[`fiware`](https://stackoverflow.com/questions/tagged/fiware) tag on Stack
Overflow.

## License

[Apache License 2.0](https://github.com/SEAMWARE/coraine/blob/main/LICENSE) —
Copyright 2026 Seamware.
