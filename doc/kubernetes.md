# Kubernetes

What a coraine deployment on Kubernetes needs: the image, the memory settings, the probes, the stop,
and the metrics. Two example manifests - `mongoc` as a Deployment, a persistent `corDB` as a
StatefulSet. Every option used here is described in [Installation & Administration](installation.md).

## The image

`quay.io/seamware/coraine:<version>-<date>-<commit>` - one immutable tag per merge to `main`, for
example `0.4.0-2026-10-08-e8bd0edc`. There is no `latest` tag: name the tag you tested. The images are
multi-arch (linux/amd64 and linux/arm64): the same tag pulls the image for the node's architecture;
`<tag>-amd64` and `<tag>-arm64` pin one. The arm64 image is built without the DDS bridge.

The image's entry point runs the broker in the foreground (`coraine -fg`), with the container's
arguments appended: `args:` in a manifest replaces the image's default arguments
(`--database mongoc --troe none --apiPlugins admin`). Every option can also be given as an environment
variable - `CORAINE_` + the long option in upper case (see
[Environment variables](installation.md#environment-variables)).

## Memory

| Setting | Why |
|---|---|
| `resources.requests.memory` equal to `resources.limits.memory` | the pod is in the Guaranteed QoS class (with CPU set the same way): the last to be evicted when the node runs short of memory |
| no `--memoryLimit` | the broker reads the container's limit from its cgroup (`memory.max`) and sets its [memory budget](installation.md#memory-budget) to 85% of it |
| `--memoryLimit <MiB>` | a budget of its own, when 85% is not the margin wanted. Above the container's limit the broker logs a warning: the kernel kills first |
| `MALLOC_ARENA_MAX=2` | glibc's per-thread arenas keep freed memory resident; measured with `corDB`, 10 000 entities took 67 MiB instead of 89, 100 000 took 335 MiB instead of 376 ([Performance](performance.md)). Not measured for throughput with many threads, so it is not the image's default |

Over the budget the broker refuses requests (503 + `Retry-After`) instead of growing until the kernel
kills it - with `corDB`, a kill takes the store's RAM with it, and a persistent store then has to be
loaded again. What counts against the budget is the broker's anonymous and shared memory: not the
`corDB` log, which is memory-mapped, file-backed page cache that the kernel reclaims.

Sizing with `corDB`: about 3.5 KiB resident per entity of five attributes - 100 000 entities are about
355 MiB, a million about 3.5 GiB ([Performance](performance.md)).

With a CPU limit, the broker's HTTP event loops and its worker pool follow the CPUs the container may
use (its CPU affinity and its cgroup quota), not the node's core count.

## Probes

Start the broker with **`--healthPort <port>`** and point the probes at that port:

```yaml
livenessProbe:
  httpGet: { path: /live, port: health }
  periodSeconds: 10
  timeoutSeconds: 2
  failureThreshold: 3
readinessProbe:
  httpGet: { path: /ready, port: health }
  periodSeconds: 5
  timeoutSeconds: 2
  failureThreshold: 2
```

**Not on 1026.** A probe on the API port is an HTTP request like any other: it waits in the same queue
as the requests, behind them. Under load it times out, the liveness probe fails, and Kubernetes
restarts a broker that was only busy - in the middle of the load. The health port is served by a
thread of its own, outside the HTTP server, and answers from figures kept current in the background:
nothing it answers waits for a request or a database.

**`/live` never fails for memory or a database.** It answers 503 only when a request has been in flight
for longer than `--healthStallTimeout` seconds (default 30) and no request has finished in that
time - the broker is stuck. A broker over its memory budget, or one whose MongoDB or PostgreSQL does
not answer, is not cured by a restart: a restart empties the caches, drops every connection and, with
`corDB`, loads the whole store again. Those make **`/ready`** answer 503, and Kubernetes takes the pod
out of the Service's endpoints until they pass.

`/ready` answers 503 while the store loads at startup, while the current-state or the temporal store
does not answer its ping, over the hard memory budget, while stuck, and from SIGTERM on. No
`startupProbe` is needed: the health port opens before the store loads, so `/live` answers 200 through
a long load of a persistent `corDB`, and `/ready` turns 200 when it is done.

A `tcpSocket` probe on the health port works too (it connects and closes; the broker closes it
unanswered) but tells only that the port is open.

Without `--healthPort`, nothing listens - the image's Docker `HEALTHCHECK` asks `GET /version` on 1026,
which is exempt from the memory budget.

## Stopping

On SIGTERM the broker answers `/ready` with 503, stops taking requests, lets the requests in flight
finish, and closes the store.

A persistent `corDB` (`--dbDir`) then syncs every tenant's log and cuts it to its records - from then on
every acknowledged write is on the disk - and writes a snapshot per tenant. The snapshot only makes the
next start faster: it loads the snapshot instead of replaying the log. A broker killed during its
snapshots (`terminationGracePeriodSeconds` over, then SIGKILL) has lost nothing; it replays the log on
the next start ([corDB's persistence](https://github.com/SEAMWARE/corDB/blob/main/doc/persistence.md),
§ 5a). Give it a grace period long enough for the snapshot of the store it holds when the start time
matters.

## Metrics

`GET /metrics` on 1026, in the Prometheus text format - served by the `admin` API plugin: without
`--apiPlugins admin` it answers 404. It is exempt from the memory budget. With the common
`prometheus.io/*` annotations:

```yaml
metadata:
  annotations:
    prometheus.io/scrape: "true"
    prometheus.io/port:   "1026"
    prometheus.io/path:   /metrics
```

or, with the Prometheus Operator, a `ServiceMonitor` on the Service's `api` port and path `/metrics`.
The memory budget is there as `ngsild_memory_budget_bytes`, `ngsild_memory_used_bytes`,
`ngsild_memory_resident_bytes` and `ngsild_requests_refused_memory_total`.

## mongoc - a Deployment

The store is MongoDB, outside the pod; the broker keeps no state of its own that a restart loses. More
than one replica needs a MongoDB replica set and `--high-availability mongo`, which keeps the
instances' caches of subscriptions, registrations and @contexts in step - see
[High Availability](high-availability.md). Without it, a subscription created through one instance is
unknown to the others.

```yaml
apiVersion: v1
kind: Secret
metadata:
  name: coraine-mongo
stringData:
  uri: "mongodb://coraine:CHANGE-ME@mongo-0.mongo,mongo-1.mongo,mongo-2.mongo/?replicaSet=rs0&authSource=admin"
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: coraine
spec:
  replicas: 1                       # more than one: --high-availability mongo (high-availability.md)
  selector:
    matchLabels: { app: coraine }
  template:
    metadata:
      labels: { app: coraine }
    spec:
      containers:
        - name: coraine
          image: quay.io/seamware/coraine:0.4.0-2026-10-08-e8bd0edc
          args:
            - --database
            - mongoc
            - --dbURI
            - $(MONGO_URI)
            - --troe
            - none
            - --apiPlugins
            - admin
            - --healthPort
            - "8081"
          env:
            - name: MONGO_URI
              valueFrom:
                secretKeyRef: { name: coraine-mongo, key: uri }
            - name: MALLOC_ARENA_MAX
              value: "2"
          ports:
            - { name: api,    containerPort: 1026 }
            - { name: health, containerPort: 8081 }
          resources:
            requests: { cpu: "2", memory: 1Gi }
            limits:   { cpu: "2", memory: 1Gi }
          livenessProbe:
            httpGet: { path: /live, port: health }
            periodSeconds: 10
            timeoutSeconds: 2
            failureThreshold: 3
          readinessProbe:
            httpGet: { path: /ready, port: health }
            periodSeconds: 5
            timeoutSeconds: 2
            failureThreshold: 2
---
apiVersion: v1
kind: Service
metadata:
  name: coraine
spec:
  selector: { app: coraine }
  ports:
    - { name: api, port: 1026, targetPort: api }
```

The health port is not in the Service: the kubelet probes the pod directly.

## corDB on disk - a StatefulSet

The store is in the broker's process, kept on a volume with `--dbDir`. One replica: a second would be
a second, separate store. The volume is `ReadWriteOnce` - one node, one broker.

```yaml
apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: coraine
spec:
  serviceName: coraine
  replicas: 1                       # the store is the process's: never more than one
  selector:
    matchLabels: { app: coraine }
  template:
    metadata:
      labels: { app: coraine }
    spec:
      terminationGracePeriodSeconds: 120
      containers:
        - name: coraine
          image: quay.io/seamware/coraine:0.4.0-2026-10-08-e8bd0edc
          args:
            - --database
            - corDB
            - --dbDir
            - /data
            - --troe
            - none
            - --apiPlugins
            - admin
            - --healthPort
            - "8081"
          env:
            - name: MALLOC_ARENA_MAX
              value: "2"
          ports:
            - { name: api,    containerPort: 1026 }
            - { name: health, containerPort: 8081 }
          resources:
            requests: { cpu: "2", memory: 4Gi }
            limits:   { cpu: "2", memory: 4Gi }
          livenessProbe:
            httpGet: { path: /live, port: health }
            periodSeconds: 10
            timeoutSeconds: 2
            failureThreshold: 3
          readinessProbe:
            httpGet: { path: /ready, port: health }
            periodSeconds: 5
            timeoutSeconds: 2
            failureThreshold: 2
          volumeMounts:
            - { name: data, mountPath: /data }
  volumeClaimTemplates:
    - metadata:
        name: data
      spec:
        accessModes: [ ReadWriteOnce ]
        resources:
          requests: { storage: 20Gi }
---
apiVersion: v1
kind: Service
metadata:
  name: coraine
spec:
  selector: { app: coraine }
  ports:
    - { name: api, port: 1026, targetPort: api }
```

The disk holds the log segments (up to 1 GiB each, 64 MiB with `--dbCompress`) and the snapshots;
100 000 entities of four attributes are 38 MB as a log or as a snapshot. With `--troe corDB` the
temporal history is on the same volume. The other `corDB` options - `--dbSync`, `--dbSnapshotEvery`,
`--dbCompress` - are in [corDB on disk](installation.md#cordb-on-disk).
