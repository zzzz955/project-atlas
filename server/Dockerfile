# syntax=docker/dockerfile:1.7
#
# project-atlas server image - multi-stage (architecture-design.md §15.2 / §15.3).
# 🔴 Build context is server/, not the repo root:
#     docker build -f server/Dockerfile server
#
# 🔴 §15.3: a Docker layer cache will happily ship yesterday's binary. That is why the runtime
# stage writes /app/VERSION, why the entrypoint prints it before anything else, and why checking
# it is part of the deploy procedure, not an optional nicety:
#     docker run --rm --entrypoint cat <image> /app/VERSION
#
# 🔴 하나의 이미지가 role 4종을 모두 담고, 실행 대상은 ATLAS_ROLE 로 고른다 (§15.3 · §5.2).
# 매핑과 실패 규약은 scripts/entrypoint.sh 에 있다.

# ---------------------------------------------------------------------------- builder
FROM ubuntu:24.04 AS builder

# 🔴 libtirpc-dev / libncurses-dev 는 vcpkg `libmysql` 포트의 시스템 전제조건이다. 없으면 configure
# 가 `Package 'libtirpc', required by 'virtual:world', not found` (cmake/rpc.cmake) 로 죽는다.
# ubuntu:24.04 는 둘 다 기본 포함하지 않는다.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git curl zip unzip tar pkg-config ca-certificates \
        libtirpc-dev libncurses-dev \
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

# 런타임 설정(§5.4). 빌드 뒤에 복사해서 ini 한 줄 고쳤다고 컴파일 캐시가 날아가지 않게 한다.
# 🔴 시크릿은 여기 없다 — .env 는 .dockerignore 로 막혀 있고 compose 가 런타임에 주입한다.
COPY server.ini ./

# ---------------------------------------------------------------------------- runtime
FROM ubuntu:24.04 AS runtime

ARG ATLAS_GIT_SHA=unknown
ARG ATLAS_BUILD_TIME=unknown

RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 🔴 배포 이미지에 테스트 바이너리를 넣지 않는다. 테스트는 CI 게이트(§15.4)의 일이고, 여기 있으면
# 배포 이미지가 실행하는 것이 서버가 아니게 된다. FE / GAME / WORLD 실행 파일은 후속 슬라이스이며
# 그때 여기에 COPY 로 추가된다.
#
# 🔴 아래 COPY 는 캐시 편의가 아니라 구조다: runtime 이 builder 에서 아무것도 가져가지 않으면
# BuildKit 이 builder 스테이지를 통째로 스킵해서, 한 줄도 컴파일하지 않은 이미지가 나온다.
COPY --from=builder /src/server.ini /app/server.ini

RUN printf 'revision=%s\nbuilt=%s\n' "$ATLAS_GIT_SHA" "$ATLAS_BUILD_TIME" > /app/VERSION

LABEL org.opencontainers.image.title="project-atlas" \
      org.opencontainers.image.revision="$ATLAS_GIT_SHA"

# 🔴 role 분기는 entrypoint 가 한다(§15.3). role 마다 이미지를 굽지 않는다 — 그러면 §5.2의
# "WORLD 와 InterWorld 는 동일 바이너리" 가 이미지 층에서 깨진다.
# 🔴 --chmod 로 실행 비트를 준다. Windows 체크아웃은 파일 모드를 들고 오지 못한다.
# 🔴 ENV ATLAS_ROLE 기본값을 두지 않는다 — 미설정은 기동 실패여야 한다(빈 값도 마찬가지).
COPY --chmod=755 scripts/entrypoint.sh /app/entrypoint.sh

ENTRYPOINT ["/app/entrypoint.sh"]
