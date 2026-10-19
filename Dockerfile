# TrinityCore "Forever" (WoW Forever beta 1.60.1.70450, CLIENT_BUILD) - https://github.com/advocaite/TrinityCore/tree/forever
#
# Stage 1: compile worldserver + bnetserver
# Stage 2: download the Forever database release (world + hotfixes)
# Stage 3: slim runtime image

ARG UBUNTU_VERSION=24.04

############################ build ############################
FROM ubuntu:${UBUNTU_VERSION} AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates git cmake make gcc g++ clang \
        libmysqlclient-dev libssl-dev libbz2-dev libreadline-dev libncurses-dev libboost-all-dev \
    && rm -rf /var/lib/apt/lists/*

ARG TC_REPO=https://github.com/advocaite/TrinityCore.git
ARG TC_REF=forever
RUN git clone --branch "${TC_REF}" --single-branch --depth 1 "${TC_REPO}" /src/TrinityCore

# Fixes not merged upstream yet (docker/patches/*.patch, applied in name order; the build fails if one no longer applies)
COPY docker/patches/ /patches/
RUN for p in /patches/*.patch; do [ -e "$p" ] || continue; echo "applying $p"; git -C /src/TrinityCore apply "$p" || exit 1; done

# BUILD_JOBS: lower it on small machines (GCC needs ~2 GB RAM per job on the biggest files)
ARG BUILD_JOBS=4
# TOOLS=1: also build mapextractor / vmap4extractor / vmap4assembler / mmaps_generator
ARG TOOLS=1
WORKDIR /src/TrinityCore/build
RUN cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/trinitycore \
        -DTOOLS=${TOOLS} \
        -DWITH_WARNINGS=0 \
    && make -j"${BUILD_JOBS}" \
    && make install

# CascLib opens every CASC file O_RDWR on Linux, which fails (EROFS) on the read-only client mount used by the
# extractor service. Fall back to O_RDONLY: the extractors only read. (Separate layer: incremental rebuild only.)
RUN sed -i 's|handle = open(szFileName, oflag \| O_LARGEFILE);|handle = open(szFileName, oflag \| O_LARGEFILE);\n        if(handle == -1 \&\& oflag == O_RDWR \&\& (errno == EROFS \|\| errno == EACCES))\n            handle = open(szFileName, O_RDONLY \| O_LARGEFILE);|' \
        /src/TrinityCore/dep/CascLib/src/common/FileStream.cpp \
    && grep -q "errno == EROFS" /src/TrinityCore/dep/CascLib/src/common/FileStream.cpp \
    && make -j"${BUILD_JOBS}" \
    && make install

############################ database release ############################
FROM ubuntu:${UBUNTU_VERSION} AS tdb

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl p7zip-full \
    && rm -rf /var/lib/apt/lists/*

ARG TDB_URL=https://github.com/advocaite/TrinityCore/releases/download/forever-tdb-2026-10-14/TDB_full_forever_2026_10_14.7z
WORKDIR /tdb
RUN curl -fL --retry 3 -o tdb.7z "${TDB_URL}" \
    && 7z e -y tdb.7z '*.sql' -r \
    && rm tdb.7z \
    # the dump creates its views with DEFINER=`trinity`@`localhost`: as any other account (e.g. trinity@'%' in the
    # mysql container) that needs SUPER/SET_USER_ID and the import stops (error 1227). Without the clause the views
    # belong to the importing user.
    && sed -i -E 's#/\*!50013 DEFINER=`[^`]*`@`[^`]*` SQL SECURITY DEFINER \*/##' /tdb/*.sql \
    && ! grep -l 'DEFINER=' /tdb/*.sql \
    && ls -la /tdb

############################ runtime ############################
FROM ubuntu:${UBUNTU_VERSION}

ARG DEBIAN_FRONTEND=noninteractive
# mysql-client is required: the TrinityCore DB updater shells out to the `mysql` binary
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates tzdata mysql-client \
        libmysqlclient21 libssl3t64 libbz2-1.0 libreadline8t64 libncurses6 \
        libboost-filesystem1.83.0 libboost-program-options1.83.0 libboost-regex1.83.0 \
        libboost-locale1.83.0 libboost-thread1.83.0 libboost-iostreams1.83.0 \
    && rm -rf /var/lib/apt/lists/*

LABEL org.opencontainers.image.source="https://github.com/senkusx/forever_docker"

COPY --from=build /opt/trinitycore /opt/trinitycore
# The updater looks for sql/ under the source directory baked in at build time
COPY --from=build /src/TrinityCore/sql /src/TrinityCore/sql
# worldserver's auto-setup imports TDB_full_*.sql from its working directory
COPY --from=tdb /tdb/ /opt/trinitycore/bin/
COPY docker/entrypoint.sh docker/extract.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/entrypoint.sh /usr/local/bin/extract.sh \
    && mkdir -p /opt/trinitycore/data /opt/trinitycore/logs /opt/trinitycore/certs /opt/trinitycore/overrides

ENV LD_LIBRARY_PATH=/opt/trinitycore/lib
WORKDIR /opt/trinitycore/bin
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["worldserver"]
