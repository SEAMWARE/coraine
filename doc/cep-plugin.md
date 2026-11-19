# The rule engine in the broker

[corCep](https://github.com/SEAMWARE/corCep) is an NGSI-LD rule engine: rules are entities of type `Rule`
("when this switch is pressed, toggle that light"; patterns, absences, schedules, conditions), and it carries out
their actions as NGSI-LD requests. It runs on its own, against any NGSI-LD broker, or inside coraine as the bridge
plugin `cep`:

```bash
coraine --bridges cep --cep-tenant home
```

The rules, their language and their statistics are corCep's, documented in its repository. This page is about the
plugin form: what it changes in the broker, and what it does not.


## Events in process: `inproc://`

The standalone corCep subscribes to what its rules watch and receives the notifications over HTTP, on a port of its
own. The plugin keeps the subscriptions and drops the HTTP:

- `cep.so` claims the notification scheme **`inproc`** (`BridgeDriver` ABI 10, `notifySchemes` / `notify`), the way
  `mqtt.so` claims `mqtt` and `mqtts`.
- corCep's subscriptions name `inproc://cep/notify/<secret>/<n>`: the scheme says *in process*, the host names the
  consumer (`cep`), the path is the standalone's (a secret made at every start, the rule's watch).
- The broker matches every write against them as against any subscription - `q`, `watchedAttributes`, the tenant,
  the owner's rights - and delivers the notification to the plugin **after the response** (the post-response
  hook), never inside the write that triggered it. The plugin queues it for the engine's own thread.
- Without the plugin loaded, a subscription naming `inproc://` is refused at creation, as one naming `mqtt://`
  without `mqtt.so` is.

`inproc` is the name of the transport, not of the product (as in ZeroMQ's `inproc://`): another in-process consumer
would claim it too, under another host.


## Requests as before

The engine's reads (the rules, an action's targets, a condition's entities), its actions (PATCH, POST, DELETE) and
its statistics are NGSI-LD requests to the broker - today over HTTP, to `--cep-broker` (default
`http://127.0.0.1:1026`). So the broker's authorization applies to them unchanged: corCep's token
(`--cep-token-env`, `--cep-token-command`), and, with `--cep-pdp`, each rule author's rights. An in-process path for
the requests is a later step, taken only if a measurement asks for it.


## Options

corCep's options, prefixed `--cep-`, added to the broker's command line and to `coraine -u` under
`Bridge (cep) plugin options:`: `--cep-broker`, `--cep-tenant`, `--cep-token-env`, `--cep-token-command`,
`--cep-stats-interval`, `--cep-renew-interval`, `--cep-queue-max`, `--cep-retry-for`, `--cep-state`, `--cep-pdp`,
`--cep-author-jwks`, `--cep-author-issuer`, `--cep-jwks-refresh`. corCep's log lines go to the broker's log (shown
with `-v`).

Not in the plugin: `/dry-run` (the standalone's HTTP endpoint for rule editors) and `--broker-cert`.


## Start and stop

- The bridges are started before the broker listens: the engine's first request is tried again (200 ms, doubling
  to 5 s) on its own thread, and the subscriptions of a previous run are removed then.
- At the broker's stop, the plugin's close wakes the engine at once: the actions still queued are sent (2 s at
  most), the pattern state (`--cep-state`) and the statistics written, and every thread of corCep's ended before
  the broker unloads the plugin.


## Built against this broker

`cep.so` links none of the cor-libs: it uses the broker's (exported). It must therefore be built against the very
checkout of them the broker was built from - which the [corLibs](https://github.com/SEAMWARE-cor/corLibs) umbrella
does (`make install` builds it last, `plugin-install` in corCep, into `/opt/seamware/plugins/bridge`). The image
carries it, and its build loads it once (`coraine --bridges cep -U`).

corCep's own tests run in both forms: `make functest` (the standalone) and `make functest-plugin` (the same tests,
`cep.so` in a coraine), each under valgrind too.
