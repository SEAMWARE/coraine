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

#
# ⚠ BEFORE THE FIRST FROM, and it has to be: an ARG declared after a FROM
# belongs to that stage, and a later FROM cannot see it - it resolves to blank
# and the build fails with "base name should not be blank", naming a line that
# looks perfectly correct.
#
#
# ⚠ THIS TAG HAS TO CARRY /opt/dds. The CI image grew the eProsima stack in
# corLibs' Dockerfile.ci section 3a, and a tag from before that builds the
# broker fine and then fails on a missing /opt/dds/include. It moves together
# with the `container:` image in .github/workflows/ci.yml - the same image,
# used for the two things the stack is for.
#
ARG CI_IMAGE=quay.io/seamware/coraine-ci:2026-09-22-f5a4eb4

#
# The Ubuntu every stage but the DDS one is FROM. Upstream's name here, for a build anywhere; CI's
# image builds (deploy.yml, release.yml, the nightly) pass the same image from the Docker Hub mirror,
# quay.io/seamware/coraine-ci:mirror-ubuntu-26.04 - same digest, no anonymous Docker Hub pull
# (.github/workflows/mirror-images.yml).
#
ARG BASE_IMAGE=ubuntu:26.04

#
# DDS=off - an image without the DDS bridge: the stack's stage below is then empty, nothing is taken
# from the CI image (which is amd64 only), and the bridge is not built (COR_BRIDGE_DDS=auto finds no
# headers). The arm64 image is built so.
#
ARG DDS=on

# --------------------------------------------------------------------------- #
# Stage 0 — where the DDS stack comes from
#
# Nothing runs here. The CI image is the build environment for the whole Cor
# stack, and it is where the eProsima stack is compiled - once, deliberately,
# every few months - because that build is eight minutes and paying it on every
# image build was the reason DDS lived in a separate image at all.
#
# /opt/dds/{lib,include} is that stack, staged by the CI image itself, so this
# file does not have to know that there are nine libraries or what they are
# called. See corLibs/docker/Dockerfile.ci, section 3a.
# --------------------------------------------------------------------------- #
FROM ${CI_IMAGE} AS ddsstack-on

FROM ${BASE_IMAGE} AS ddsstack-off
RUN mkdir -p /opt/dds/lib /opt/dds/include

FROM ddsstack-${DDS} AS ddsstack

# --------------------------------------------------------------------------- #
# Stage 1 — builder
# --------------------------------------------------------------------------- #
FROM ${BASE_IMAGE} AS builder

# Both of these are source-built on the dev box, so the image matches it rather
# than taking whatever apt has (apt's libmicrohttpd is a release behind, and
# mongo-c v2 — which the mongoc plugin needs for mongoc2.pc — isn't packaged).
ARG MONGOC_VERSION=2.2.2
ARG MHD_VERSION=1.0.2

ENV DEBIAN_FRONTEND=noninteractive \
    HOME=/root

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git pkg-config ca-certificates curl wrk python3 libjemalloc2 libtcmalloc-minimal4 \
      libssl-dev libgeos-dev libmosquitto-dev libpq-dev \
      libsasl2-dev libzstd-dev zlib1g-dev \
      nlohmann-json3-dev libyaml-cpp-dev \
    && rm -rf /var/lib/apt/lists/*

#
# The DDS stack, so that the bridge plugin can be built against it.
#
# nlohmann-json3-dev and libyaml-cpp-dev above are part of this and are not
# ours. They are TRANSITIVE BUILD dependencies: ddsenabler_participants/Writer.hpp
# includes <nlohmann/json.hpp> and ddspipe_yaml/Yaml.hpp includes
# <yaml-cpp/yaml.h>, so anything compiling against the Enabler needs both
# present. Their absence is confusing rather than informative, because the
# error names a file in /usr/local/include that we did not write:
#
#   /usr/local/include/ddspipe_yaml/Yaml.hpp:19:10: fatal error:
#       yaml-cpp/yaml.h: No such file or directory
#
# The LIBRARIES are a different matter and come from /opt/dds/lib with the
# rest, so that the version linked is the one Fast DDS was built against.
#
# corDdsBridge's makefile detects the Enabler and builds the plugin when it is
# there (COR_BRIDGE_DDS=auto), so nothing below has to ask for it: the umbrella
# `make -C corLibs install` walks corDdsBridge like every other lib and dds.so lands
# in /opt/seamware/plugins/bridge, which stage 2 copies wholesale.
#
COPY --from=ddsstack /opt/dds/include/ /usr/local/include/
COPY --from=ddsstack /opt/dds/lib/     /usr/local/lib/
RUN ldconfig

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
WORKDIR /root/git

# Cor-Libs are NOT cloned here: docker/vendor-libs.sh exports each one's committed
# HEAD next to this Dockerfile, so the image is built from the tree the developer
# actually has rather than from whatever `main` is at that moment. MANIFEST.txt
# records the exact SHAs that went in. Build fails here with a clear message if
# you skipped it.
COPY docker/vendor/ /tmp/vendor/
RUN set -eux; \
    test -f /tmp/vendor/MANIFEST.txt || { echo "run ./docker/vendor-libs.sh first"; exit 1; }; \
    for tar in /tmp/vendor/*.tar; do tar -xf "$tar" -C /root/git; done; \
    cat /tmp/vendor/MANIFEST.txt; \
    rm -rf /tmp/vendor

#
# BUILD - release (the default) or debug: the same stack, two images.
#
#   release  traces compiled OUT (COR_T/COR_LIB_T - corLog.h), -O2 -g. What is deployed.
#            A crash writes a report (request, payload, command line, build id, stack);
#            --traceLevels turns on nothing, and says so.
#   debug    traces compiled in - the image to REPRODUCE a crash report with, or to
#            run with --traceLevels. Tagged <tag>-debug beside the release one.
#
# The umbrella builds every lib in dependency order and collects the artifacts, in the
# flavour asked for - dds.so included (a lib without flavours ignores it).
#
ARG BUILD=release
RUN test "$BUILD" = release -o "$BUILD" = debug || { echo "BUILD must be release or debug, not '$BUILD'"; exit 1; }
RUN make -C /root/git/corLibs install BUILD=$BUILD

# The broker itself. .dockerignore keeps the host build trees out, so CMake
# starts clean instead of inheriting a cache full of host paths.
COPY . /root/git/coraine
WORKDIR /root/git/coraine

#
# release: `make pgo` - a profile-guided build (doc/performance.md, "Profile-guided"): every
# library and the broker built instrumented, trained on test/perf/pgoTrain.sh (brokers started
# right here, on local ports - wrk is in the builder for it), rebuilt with the profile. +1-10 %
# per core against the same build without it. `make di` is the Debug broker (-DCOR_T_ON) over
# the debug libs built above.
#
#
# WORKLOAD (a workload file in the build context - doc/extreme-performance.md): `make tune` instead -
# the profile trained on that workload, the knobs measured on it (here, in the builder), the best kept as
# /opt/seamware/etc/tune.env for the entry point (docker/coraine-start)
#
ARG WORKLOAD=
RUN if [ "$BUILD" = debug ]; then make di; \
    elif [ -n "$WORKLOAD" ]; then make tune WORKLOAD="$WORKLOAD" PGO_RESTORE_DEBUG=0 && make install_pgo \
      && test/perf/tune/tuneEnv.py tune-result.json > /opt/seamware/etc/tune.env && cat /opt/seamware/etc/tune.env; \
    else make pgo PGO_RESTORE_DEBUG=0 && make install_pgo; fi

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
FROM ${BASE_IMAGE}

LABEL org.opencontainers.image.title="coraine" \
      org.opencontainers.image.description="NGSI-LD Context Broker — ETSI GS CIM 009 v1.9.1" \
      org.opencontainers.image.vendor="Seamware" \
      org.opencontainers.image.licenses="Apache-2.0"

ENV DEBIAN_FRONTEND=noninteractive

# Runtime shared libraries only. curl is here for the healthcheck below.
RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl3t64 libgeos-c1t64 libmosquitto1 libpq5 \
      libsasl2-2 libzstd1 zlib1g curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/bin/coraine /usr/local/bin/coraine
# coraine-import - the writer half of a migration (doc/migration.md): every build path above installs it
COPY --from=builder /usr/local/bin/coraine-import /usr/local/bin/coraine-import
COPY --from=builder /opt/seamware/ /opt/seamware/
COPY docker/coraine-start /usr/local/bin/coraine-start

#
# The allocator `make tune` chose (WORKLOAD), and only that one
#
RUN A=$(sed -n 's/^TUNE_ALLOCATOR=//p' /opt/seamware/etc/tune.env 2>/dev/null); \
    case "$A" in \
      jemalloc)         P=libjemalloc2 ;; \
      tcmalloc_minimal) P=libtcmalloc-minimal4 ;; \
      *)                P= ;; \
    esac; \
    if [ -n "$P" ]; then apt-get update && apt-get install -y --no-install-recommends $P && rm -rf /var/lib/apt/lists/*; fi
COPY --from=builder /usr/local/lib/libmicrohttpd.so.12* /usr/local/lib/
COPY --from=builder /usr/local/lib/libmongoc* /usr/local/lib/
COPY --from=builder /usr/local/lib/libbson* /usr/local/lib/

#
# And the DDS stack, as a DIRECTORY.
#
# ⚠ Not as a glob. `COPY --from=<stage> /usr/local/lib/libfastdds.so*`
# dereferences symlinks, and libfastdds.so, .so.3.6 and .so.3.6.2.0 are one
# 16 MiB library and two links to it - so a globbed copy writes it three times.
# Nine of them turned 21 MiB of libraries into 63 MiB of image, and it looked
# exactly like the price of DDS.
#
COPY --from=ddsstack /opt/dds/lib/ /usr/local/lib/

#
# The bridge's CONFIG FILE, and the directory it names.
#
# The broker exits when --bridgeConfig names a file it cannot read, which is
# right - a bridge that silently carries no topics is worse than one that does
# not start - but it means the image has to ship something at the path the
# documentation and docker-compose-dds.yml both use. This is that something:
# domain 0, no topics. It joins the domain and carries nothing, which is the
# honest default, and a deployment mounts its own file straight over it:
#
#     volumes: [ ./dds.json:/opt/seamware/etc/dds.json:ro ]
#
# so nothing on the command line changes and the same image serves every
# deployment.
#
# The types directory is the broker's own and is written to: types are
# announced by the participants on the domain and kept, so that a restarted
# broker is not blind until each of them happens to announce itself again.
#
# ⚠ No VOLUME on it, unlike the coraine-dds image this replaced. A VOLUME here
# would hand an anonymous volume to every container started from this image,
# including the overwhelming majority that never pass --bridges dds. A
# deployment that wants the types to survive a restart names a volume itself,
# which is what docker-compose-dds.yml does.
#
COPY etc/dds-default.json /opt/seamware/etc/dds.json
RUN mkdir -p /opt/seamware/etc/dds-types

RUN ldconfig \
    && if ldd /opt/seamware/plugins/bridge/dds.so | grep "not found"; then \
         echo "dds.so has unresolved libraries - see above"; \
         exit 1; \
       fi \
    && echo "dds bridge plugin present, and it resolves every library it needs"

EXPOSE 1026

# /version: answered without touching the store, and exempt from the memory budget - over the budget
# everything else is refused with 503 by design, and a full broker is not an unhealthy one. (A broker
# started with --healthPort has a better probe: GET /live on that port, outside the HTTP server's
# queue - doc/kubernetes.md.)
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
  CMD curl -fsS http://localhost:1026/version >/dev/null || exit 1

# coraine -fg (mandatory in a container: without it the broker daemonizes and PID 1 exits), with what
# `make tune` chose if the image was built with a WORKLOAD (docker/coraine-start)
ENTRYPOINT ["coraine-start"]

# Overridable. Default is the Mongo backend, matching the dev/test default;
# pass --database corDB to run with no external services.
CMD ["--database", "mongoc", "--troe", "none", "--apiPlugins", "admin"]
