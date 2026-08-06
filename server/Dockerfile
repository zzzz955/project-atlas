# syntax=docker/dockerfile:1.7
#
# project-atlas server image - multi-stage (architecture-design.md §15.2 / §15.3).
# 🔴 Build context is server/, not the repo root:
#     docker build -f server/Dockerfile server
#
# 🔴 §15.3: a Docker layer cache will happily ship yesterday's binary. That is why the runtime
# stage writes /app/VERSION and why checking it is part of the deploy procedure, not an optional
# nicety. After deploying:  docker run --rm <image> cat /app/VERSION

# ---------------------------------------------------------------------------- builder
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git curl zip unzip tar pkg-config ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# vcpkg is cloned inside the image; the host clone is excluded by .dockerignore. The exact port
# versions come from vcpkg.json's builtin-baseline, so dev and prod resolve identically.
RUN git clone https://github.com/microsoft/vcpkg.git /src/vcpkg \
    && /src/vcpkg/bootstrap-vcpkg.sh -disableMetrics

COPY vcpkg.json CMakePresets.json CMakeLists.txt ./
COPY atlas ./atlas
COPY generated ./generated
COPY tests ./tests

# 🔴 §15.2: a container has a fresh filesystem every time, so vcpkg's default local binary cache
# buys nothing. Mount it explicitly or every build recompiles Boost from source.
RUN --mount=type=cache,target=/root/.cache/vcpkg/archives \
    cmake --preset linux-release && cmake --build --preset linux-release

# ---------------------------------------------------------------------------- runtime
FROM ubuntu:24.04 AS runtime

ARG ATLAS_GIT_SHA=unknown
ARG ATLAS_BUILD_TIME=unknown

RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# The framework currently builds static libraries and the test binary; the FE / GAME / WORLD
# executables land in a later slice and are added here alongside it.
COPY --from=builder /src/build/linux-release/tests/atlas_core_tests /app/atlas_core_tests

RUN printf 'revision=%s\nbuilt=%s\n' "$ATLAS_GIT_SHA" "$ATLAS_BUILD_TIME" > /app/VERSION

LABEL org.opencontainers.image.title="project-atlas" \
      org.opencontainers.image.revision="$ATLAS_GIT_SHA"

# Default command is the version check + the test binary: running the image proves which build it
# actually contains.
CMD ["/bin/sh", "-c", "cat /app/VERSION && exec /app/atlas_core_tests"]
