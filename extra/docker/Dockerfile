# syntax=docker/dockerfile:1
# MaNGOS Three -- one build, two images.
#
#   docker build -f extra/docker/Dockerfile --target mangosd -t mangosthree-mangosd .
#   docker build -f extra/docker/Dockerfile --target realmd  -t mangosthree-realmd  .
#
# Both servers are compiled once in the `build` stage; each runtime target copies only
# what it needs. Every apt step is a single layer without `apt-get upgrade`: the package
# index and the packages are fetched together, so a mirror mid-sync cannot break the
# build later from a stale cached index (which is what took the old images down).

FROM ubuntu:22.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential clang cmake ninja-build ca-certificates \
        libbz2-dev libmysqlclient-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# The two servers only -- deliberately NOT the same feature set as the Linux CI build,
# which additionally builds the tools (BUILD_TOOLS=1) and the test suite (WITH_TESTS=1).
# Neither belongs in a runtime image: the extractor runs against a game client, not a
# server, and the tests are CI's business. CI is what proves this source tree compiles
# and passes; this stage only has to produce the two daemons.
# WITHOUT_GIT because .dockerignore excludes .git; the OCI revision label carries the
# commit instead. The configs need no flag: they install to <prefix>/etc.
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_INSTALL_PREFIX=/mangos -DWITHOUT_GIT=1 \
        -DBUILD_MANGOSD=1 -DBUILD_REALMD=1 -DBUILD_TOOLS=0 \
        -DSCRIPT_LIB_SD3=1 -DPCH=1 \
    && cmake --build build --parallel \
    && cmake --install build \
    && for bin in /mangos/bin/mangosd /mangos/bin/realmd; do \
           if readelf -d "$bin" | grep -E 'NEEDED.*lib(ssl|crypto)'; then \
               echo "$bin links the removed crypto library" >&2; exit 1; \
           fi; \
           if nm --defined-only "$bin" | grep -E ' (CRYPTO_malloc|EVP_MD_CTX_new|BN_new|ERR_get_error)$'; then \
               echo "$bin carries a static copy of the removed crypto library" >&2; exit 1; \
           fi; \
       done

FROM ubuntu:22.04 AS runtime-base
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libmysqlclient21 libbz2-1.0 zlib1g ca-certificates tzdata gosu \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -g 1000 mangos \
    && useradd -u 1000 -g mangos -d /mangos -s /usr/sbin/nologin mangos \
    && mkdir -p /mangos/bin /mangos/etc /mangos/data /mangos/logs /mangos/dist \
    && chown -R mangos:mangos /mangos
ENV TZ=Etc/UTC \
    MANGOS_DATA_DIR=/mangos/data \
    MANGOS_LOGS_DIR=/mangos/logs
COPY --chmod=755 extra/docker/docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
VOLUME ["/mangos/etc", "/mangos/data", "/mangos/logs"]
WORKDIR /mangos/bin
ENTRYPOINT ["docker-entrypoint.sh"]

FROM runtime-base AS mangosd
COPY --from=build --chown=mangos:mangos /mangos/bin/mangosd /mangos/bin/
COPY --from=build /mangos/etc/mangosd.conf.dist /mangos/etc/ahbot.conf.dist /mangos/dist/
RUN if ldd /mangos/bin/mangosd | grep 'not found'; then echo 'mangosd: unresolved shared libraries' >&2; exit 1; fi
EXPOSE 8085
CMD ["mangosd"]

FROM runtime-base AS realmd
COPY --from=build --chown=mangos:mangos /mangos/bin/realmd /mangos/bin/
COPY --from=build /mangos/etc/realmd.conf.dist /mangos/dist/
RUN if ldd /mangos/bin/realmd | grep 'not found'; then echo 'realmd: unresolved shared libraries' >&2; exit 1; fi
EXPOSE 3724
CMD ["realmd"]
