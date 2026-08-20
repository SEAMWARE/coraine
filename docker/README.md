# coraine in Docker

The image runs the broker with no external service required (`--database corDB`),
or against MongoDB (`--database mongoc`, the default).

## Build

```sh
docker build -f docker/Dockerfile \
  --build-arg GIT_SHA=$(git rev-parse --short HEAD) \
  --build-arg BUILD_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ) \
  -t coraine:local .
```

`GIT_SHA` and `BUILD_AT` are stamped into the provenance the broker reports on
`/info/sourceIdentity`. They are passed in because `.git` is not part of the build
context.

## Run

```sh
# In-memory, nothing else needed
docker run --rm -p 1026:1026 coraine:local --database corDB --apiPlugins admin

# Against MongoDB
docker run --rm -p 1026:1026 coraine:local --database mongoc --dbHost mongo
```

## docker-compose

- `docker/docker-compose.yml` — broker + MongoDB.
- `docker/docker-compose-distops.yml` — several brokers, for distributed operations.

```sh
docker compose -f docker/docker-compose.yml up
```

## Configuration

Every option is a command-line argument; `coraine --usage` lists them all, including
the arguments contributed by whichever plugins are selected. The one environment
variable the broker reads is **`SEAMWARE_PLUGIN_DIR`**, which overrides where plugins
are loaded from (default `/opt/seamware/plugins`).

The port is not fixed in the image: pass `--port` and publish what you chose.

## Tags

Published images will be `quay.io/seamware/coraine:<version>`, versioned with SemVer;
publishing runs from CI, which is not enabled yet. Until then, build locally as above.
See [QUAY.md](QUAY.md) for the procedure.
