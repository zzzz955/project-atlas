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

# 🔴 libtirpc-dev / libncurses-dev 는 여기 없다. 그 둘은 vcpkg `libmysql` 포트의 시스템
# 전제조건이었고, §15.5(2026-08-10)에서 DB 클라이언트를 `libmariadb` 로 교체하면서 사라졌다 —
# libmariadb 는 Sun RPC 도 ncurses 도 요구하지 않는다. 되살리지 않는다: 안 쓰는 시스템 패키지는
# 빌더 레이어를 키우고, 왜 있는지 아무도 모르는 줄이 된다.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git curl zip unzip tar pkg-config ca-certificates \
        autoconf autoconf-archive automake libtool \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# vcpkg is cloned inside the image; the host clone is excluded by .dockerignore. The exact port
# versions come from vcpkg.json's builtin-baseline, so dev and prod resolve identically.
RUN git clone https://github.com/microsoft/vcpkg.git /src/vcpkg \
    && /src/vcpkg/bootstrap-vcpkg.sh -disableMetrics

COPY vcpkg.json CMakePresets.json CMakeLists.txt ./
COPY atlas ./atlas
COPY generated ./generated
# 🔴 게임 계층(§14). 코어 순수성 게이트(§15.4)가 `item`/`inventory`/`equip` 를 server/atlas/**
# 밖으로 밀어냈으므로 별도 디렉터리이고, 루트 CMakeLists 가 add_subdirectory(game) 로 잡는다 —
# 여기서 빠지면 configure 가 실패한다.
COPY game ./game
# 🔴 부하 하네스(§16.1). 배포 산출물이 아니라 클라이언트이지만 루트 CMakeLists 가
# add_subdirectory(loadgen) 으로 잡으므로 여기서 빠지면 configure 가 실패한다. 아래 runtime
# 스테이지는 이 바이너리를 복사하지 않는다 — 서버 이미지 안의 부하 생성기는 공격자의 도구다.
COPY loadgen ./loadgen
# 🔴 데모용 콘솔 클라이언트. loadgen 과 같은 이유로 여기 있다 — 루트 CMakeLists 가
# add_subdirectory(console_client) 로 잡으므로 빠지면 configure 가 실패한다. runtime 스테이지는
# 이것도 복사하지 않는다: 서버 이미지에 클라이언트를 넣을 이유가 없다.
COPY console_client ./console_client
COPY tests ./tests

# 🔴 §15.2: a container has a fresh filesystem every time, so vcpkg's default local binary cache
# buys nothing. Mount it explicitly or every build recompiles Boost from source.
ARG ATLAS_GIT_SHA=unknown
RUN --mount=type=cache,target=/root/.cache/vcpkg/archives \
    cmake --preset linux-release -DATLAS_BUILD_ID="${ATLAS_GIT_SHA}" \
    && cmake --build --preset linux-release \
    && objcopy --only-keep-debug build/linux-release/game/atlas_game \
        build/linux-release/game/atlas_game.debug \
    && objcopy --strip-debug build/linux-release/game/atlas_game \
    && objcopy --add-gnu-debuglink=build/linux-release/game/atlas_game.debug \
        build/linux-release/game/atlas_game

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
# 배포 이미지가 실행하는 것이 서버가 아니게 된다.
#
# 🔴 GAME 만 들어온다. FE / WORLD 실행 파일은 아직 없고, 없는 것을 있는 척 감싸지 않는다(§15.3) —
# entrypoint 는 그 두 role 에 대해 계속 exit 69 로 죽는다.
COPY --from=builder /src/build/linux-release/game/atlas_game /app/atlas_game
# 🔴 DWARF is kept outside the runtime image. Core dumps can contain request data and the debug
# file exposes source paths; both are incident artifacts, not public image contents. Export this
# stage by exact image revision when symbolizing: `docker build --target symbols --output ...`.
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

# ---------------------------------------------------------------------------- symbols
FROM scratch AS symbols

COPY --from=builder /src/build/linux-release/game/atlas_game.debug /atlas_game.debug
COPY --from=runtime /app/VERSION /VERSION
