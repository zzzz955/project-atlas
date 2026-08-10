#!/bin/sh
# project-atlas container entrypoint — single image, ATLAS_ROLE dispatch.
# architecture-design.md §15.3 (배포) · §5.2 (WORLD 와 InterWorld 는 동일 바이너리).
#
# 🔴 이 파일이 배포 층의 role 규약을 들고 있는 유일한 자리다. role 문자열 집합은
#    server/atlas/config/server_config.h 의 `ServerRole` 및 server.ini `[server] role` 과 1:1이다.
#
#   ATLAS_ROLE   binary             note
#   ----------   ----------------   ---------------------------------------------------------
#   fe           /app/atlas_fe      후속 슬라이스 (아직 이미지에 없음)
#   game         /app/atlas_game    ✅ 실재한다 (server/game/ · 도메인 모델 + 원자적 장착)
#   world        /app/atlas_world   후속 슬라이스
#   interworld   /app/atlas_world   🔴 world 와 **같은 바이너리**(§5.2). InterWorld 는 신규 서버가
#                                   아니라 ini 설정 변형이므로 이미지도 실행 파일도 쪼개지 않는다.
#
# 🔴 기본 role 은 없다. ATLAS_ROLE 미설정은 명시적 실패다 — 엉뚱한 서버로 조용히 뜨는 것이
#    기동 실패보다 나쁘다.
# 🔴 바이너리가 아직 없는 role 도 명시적 실패다(§15.3). 없는 것을 있는 척 감싸지 않는다.
#    game 이 실재하게 됐다고 fe/world 를 "곧 나온다"로 감싸지 않는다 — 아래 -x 검사는 그대로다.
#    🔴 이 파일이 role 을 바이너리로 고르고, 런타임 정체성은 server.ini `[server] role` 이다.
#    두 값이 어긋나면 atlas_game 이 exit 64 로 죽는다(game/main.cpp) — §5.4 의 그 경계다.
set -eu

# 🔴 §15.3: Docker 레이어 캐시는 어제 바이너리를 태연히 배포한다. 그래서 컨테이너는 무슨 일을
#    하기 전에 자기가 어느 빌드인지부터 찍는다.
cat /app/VERSION

ATLAS_ROLES='fe | game | world | interworld'

if [ -z "${ATLAS_ROLE:-}" ]; then
    echo "[entrypoint] ATLAS_ROLE is not set, and there is no default role (architecture-design.md §15.3)." >&2
    echo "[entrypoint] Allowed roles: ${ATLAS_ROLES}" >&2
    exit 64
fi

case "${ATLAS_ROLE}" in
    fe)
        ATLAS_BIN=/app/atlas_fe
        ;;
    game)
        ATLAS_BIN=/app/atlas_game
        ;;
    world | interworld)
        # 🔴 §5.2: 동일 바이너리. 구분은 server.ini 의 role / world_id / server_group 이 한다.
        ATLAS_BIN=/app/atlas_world
        ;;
    *)
        echo "[entrypoint] unknown ATLAS_ROLE '${ATLAS_ROLE}' (architecture-design.md §5.2)." >&2
        echo "[entrypoint] Allowed roles: ${ATLAS_ROLES}" >&2
        exit 64
        ;;
esac

if [ ! -x "${ATLAS_BIN}" ]; then
    echo "[entrypoint] role '${ATLAS_ROLE}' maps to ${ATLAS_BIN}, which is not in this image yet." >&2
    echo "             GAME exists; FE and WORLD land in a later slice (architecture-design.md §15.3)." >&2
    exit 69
fi

# 🔴 exec 로 PID 1 을 서버에 넘긴다 — 그래야 docker stop 의 SIGTERM 이 sh 가 아니라 서버에 닿는다.
exec "${ATLAS_BIN}" "$@"
