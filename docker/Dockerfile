# coraine — NGSI-LD Context Broker (ETSI GS CIM 009 v1.9.1)
#
# Multi-stage: stage 1 builds the whole dependency stack and the broker, stage 2
# ships only what is needed to run it.
#
# Build from the REPO ROOT (not from docker/), and stage the private libs first:
#   ./docker/vendor-libs.sh
#   docker build -f docker/Dockerfile -t coraine:local .
#
# Run with no external services at all (in-memory DB):
#   docker run --rm -p 1026:1026 coraine:local --database corDB
#
# Run against MongoDB (the default backend) — see docker-compose.yml.
#
# The dependency pins below mirror bootstrap-corlibs.sh, which is the source of
# truth for the dev-box stack. When those pins move, move these too.
#
# Copyright 2026 Seamware

# --------------------------------------------------------------------------- #
# Stage 1 — builder
# --------------------------------------------------------------------------- #
FROM ubuntu:26.04 AS builder

# Both of these are source-built on the dev box, so the image matches it rather
# than taking whatever apt has (apt's libmicrohttpd is a release behind, and
# mongo-c v2 — which the mongoc plugin needs for mongoc2.pc — isn't packaged).
ARG MONGOC_VERSION=2.2.2
ARG MHD_VERSION=1.0.2

ENV DEBIAN_FRONTEND=noninteractive \
    HOME=/root

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git pkg-config ca-certificates curl \
      libssl-dev libgeos-dev libmosquitto-dev libpq-dev libicu-dev \
      libsasl2-dev libzstd-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# libmicrohttpd — the HTTP server the REST layer sits on.
RUN curl -fsSL "https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-${MHD_VERSION}.tar.gz" -o /tmp/mhd.tar.gz \
 && tar -xzf /tmp/mhd.tar.gz -C /tmp \
 && cd "/tmp/libmicrohttpd-${MHD_VERSION}" \
 && ./configure --disable-doc --disable-examples --disable-https \
 && make -j"$(nproc)" && make install \
 && ldconfig \
 && rm -rf "/tmp/libmicrohttpd-${MHD_VERSION}" /tmp/mhd.tar.gz

# mongo-c driver v2 — required by the mongoc DB plugin (pkg-config: mongoc2).
RUN git clone --depth 1 --branch "${MONGOC_VERSION}" \
      https://github.com/mongodb/mongo-c-driver.git /tmp/mongo-c \
 && cmake -S /tmp/mongo-c -B /tmp/mongo-c/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF -DBUILD_TESTING=OFF \
 && cmake --build /tmp/mongo-c/build -j"$(nproc)" \
 && cmake --install /tmp/mongo-c/build \
 && ldconfig \
 && rm -rf /tmp/mongo-c

# The lib stack. Every lib resolves its dependencies as ../<name>, so they all
# have to be siblings — the same layout as a dev box. The directory itself is
# free to be anywhere; /root/git is simply this image's choice.
#
# k-libs are public on GitLab, so they clone straight from their pinned release
# branches. The pins are NOT restated here: docker/vendor-libs.sh stages
# corLibs/klib-pins next to the tarballs and it is the single source of truth,
# shared with corLibs/bootstrap.sh. Copied on its own, and before the tarballs,
# so a lib commit does not invalidate this clone layer.
WORKDIR /root/git
COPY docker/vendor/klib-pins /tmp/klib-pins
RUN set -eux; \
    test -f /tmp/klib-pins || { echo "run ./docker/vendor-libs.sh first"; exit 1; }; \
    sed 's/#.*//' /tmp/klib-pins | while read -r repo ref; do \
      [ -n "${repo:-}" ] || continue; \
      git clone --quiet --depth 1 --branch "${ref}" \
        "https://gitlab.com/kzangeli/${repo}.git" "/root/git/${repo}"; \
    done; \
    ls -d /root/git/k*

# Cor-Libs are PRIVATE on GitHub — a container cannot clone them. docker/vendor-libs.sh
# exports each one's committed HEAD next to this Dockerfile; MANIFEST.txt records the
# exact SHAs that went in. Build fails here with a clear message if you skipped it.
COPY docker/vendor/ /tmp/vendor/
RUN set -eux; \
    test -f /tmp/vendor/MANIFEST.txt || { echo "run ./docker/vendor-libs.sh first"; exit 1; }; \
    for tar in /tmp/vendor/*.tar; do tar -xf "$tar" -C /root/git; done; \
    cat /tmp/vendor/MANIFEST.txt; \
    rm -rf /tmp/vendor

# The umbrella builds every lib in dependency order and collects the artifacts.
RUN make -C /root/git/corLibs di

# The broker itself. .dockerignore keeps the host build trees out, so CMake
# starts clean instead of inheriting a cache full of host paths.
COPY . /root/git/coraine
WORKDIR /root/git/coraine

RUN make i

# Stamp real provenance. The makefile regenerates etc/contextSourceExtras.json on every
# build, but .git is out of the build context, so its `git rev-parse` yields "unknown"
# in here. Without this step the image serves gitSha "unknown" on sourceIdentity and bug
# reports cannot be traced to a commit. Pass the values in:
#   --build-arg GIT_SHA=$(git rev-parse --short HEAD) --build-arg BUILD_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
ARG GIT_SHA=unknown
ARG BUILD_AT=unknown
# VERSION defaults to the header the broker itself compiles in, so the image can
# never announce a version the binary disagrees with. Three hand-kept copies had
# already drifted once.
RUN set -eux; \
    VERSION="${VERSION:-$(sed -n 's/^#define CORAINE_VERSION "\(.*\)"/\1/p' \
                          /root/git/coraine/src/app/coraine/coraineVersion.h)}"; \
    printf '{\n  "version": "%s",\n  "gitSha": "%s",\n  "buildAt": "%s"\n}\n' \
      "${VERSION}" "${GIT_SHA}" "${BUILD_AT}" > /opt/seamware/etc/contextSourceExtras.json; \
    cat /opt/seamware/etc/contextSourceExtras.json

# --------------------------------------------------------------------------- #
# Stage 2 — runtime
# --------------------------------------------------------------------------- #
FROM ubuntu:26.04

LABEL org.opencontainers.image.title="coraine" \
      org.opencontainers.image.description="NGSI-LD Context Broker — ETSI GS CIM 009 v1.9.1" \
      org.opencontainers.image.vendor="Seamware" \
      org.opencontainers.image.licenses="Proprietary"

ENV DEBIAN_FRONTEND=noninteractive

# Runtime shared libraries only. curl is here for the healthcheck below.
RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl3t64 libgeos-c1t64 libmosquitto1 libpq5 libicu78 \
      libsasl2-2 libzstd1 zlib1g curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/bin/coraine /usr/local/bin/coraine
COPY --from=builder /opt/seamware/ /opt/seamware/
COPY --from=builder /usr/local/lib/libmicrohttpd.so.12* /usr/local/lib/
COPY --from=builder /usr/local/lib/libmongoc* /usr/local/lib/
COPY --from=builder /usr/local/lib/libbson* /usr/local/lib/
RUN ldconfig

EXPOSE 1026

# /ngsi-ld/v1/types needs no parameters and no data to answer 200, which makes it
# the cheapest honest liveness signal.
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
  CMD curl -fsS http://localhost:1026/ngsi-ld/v1/types >/dev/null || exit 1

# -fg is mandatory in a container: without it the broker daemonizes and PID 1 exits.
ENTRYPOINT ["coraine", "-fg"]

# Overridable. Default is the Mongo backend, matching the dev/test default;
# pass --database corDB to run with no external services.
CMD ["--database", "mongoc", "--troe", "none", "--apiPlugins", "admin"]
