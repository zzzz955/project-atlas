# project-atlas

**C++20 MMO 게임 서버 프레임워크**입니다 — 게임이 아니라 서버 코어가 결과물입니다. 클라이언트는 교체 가능하고, 데모 게임(캐릭터 1행 · 아이템 1테이블 · 장착 슬롯 3개)은 코어 경로를 실제로 태워 보기 위해서만 존재합니다.

이 저장소가 주장하는 것은 **측정된 서버 동작**입니다 — 부하 하네스, 근거와 함께 지목한 병목, 빌드를 실패시키는 게이트. 설명된 아키텍처가 아닙니다. 게임 엔진이 아니고, 라이브 서비스가 아니며, 수익은 목표가 아닙니다.

> **두 목록을 일부러 떼어 놓았습니다.** [지금 도는 것](#지금-도는-것)은 다섯 개 명령으로 띄울 수 있는 코드입니다. [아직 없는 것](#아직-없는-것)은 문서 아래쪽에 설계라고 이름 붙여 두었습니다. 아키텍처 문서는 전체 토폴로지(FE / GAME / WORLD, Actor / AoI / 행동 트리, Relay)를 서술하지만 그 대부분은 **미구현**이고, 이 README는 설계를 자산으로 세지 않습니다.

---

## 읽는 순서

코드를 처음 보신다면 아래 순서를 권합니다. 세 축(네트워크 · 비동기 · 데이터베이스)마다 **판단이 걸려 있는 파일**만 골랐습니다. 각 항목의 마지막 열은 그 판단의 근거를 적어 둔 기술 문서 슬라이드입니다.

### ① 네트워크 — 끊겨 오는 바이트에서 패킷을 복원

| 파일 | 무엇을 보실 수 있는지 | 문서 |
|---|---|---|
| [`server/atlas/proto/frame.h`](server/atlas/proto/frame.h) | 12바이트 고정 헤더(`length · opcode · seq · crc32`). 필드 단위 read/write — `#pragma pack` 미사용 | 7p |
| [`server/atlas/proto/frame_reader.cpp`](server/atlas/proto/frame_reader.cpp) | **검증 순서가 곧 방어입니다.** 상한 비교 → 실제 수신량 비교 → 그 뒤에야 버퍼 분할. 덜 도착한 패킷은 오류가 아니라 대기 | 7 – 8p |
| [`server/atlas/net/session.cpp`](server/atlas/net/session.cpp) | 접속별 순차 실행, 유휴 타임아웃. **타이머 만료 콜백이 자기가 낡은 호출인지 스스로 판별하는 구간**이 이 파일의 핵심입니다 | 9 – 10p |
| [`server/atlas/net/acceptor.cpp`](server/atlas/net/acceptor.cpp) | accept 실패 시 100 ms 후 재시도. 지수 백오프를 쓰지 않은 이유는 주석에 있습니다 | 9p |

### ② 비동기 — 스레드를 붙잡는 작업과 그렇지 않은 작업의 분리

| 파일 | 무엇을 보실 수 있는지 | 문서 |
|---|---|---|
| [`server/atlas/net/io_runner.cpp`](server/atlas/net/io_runner.cpp) | Boost.Asio I/O 스레드 16개. 소켓 처리만 담당합니다 | 12p |
| [`server/atlas/db/db_runner.h`](server/atlas/db/db_runner.h) | 동기 DB 호출을 전용 스레드 2개로 격리. **`SubmitResult`를 bool이 아니라 enum으로 둔 이유**(과부하와 서버 종료는 대응이 반대입니다) | 12 – 13p, 30p |
| [`server/atlas/core/ctx.h`](server/atlas/core/ctx.h) | 스레드 경계를 넘는 요청 정보. `is_trivially_copyable` + `sizeof <= 64`를 **컴파일 시점에 강제**합니다. 이관이 아니라 복사인 이유가 여기 있습니다 | 14p |

### ③ 데이터베이스 — DB가 대신 지켜 주지 않는 규칙

| 파일 | 무엇을 보실 수 있는지 | 문서 |
|---|---|---|
| [`server/atlas/db/connection_pool.cpp`](server/atlas/db/connection_pool.cpp) | 대여 타임아웃 5초. **세마포어 대신 뮤텍스를 쓴 이유**가 주석에 있습니다 | 13p, 16p |
| [`server/atlas/db/connection.cpp`](server/atlas/db/connection.cpp) | 재접속 시 statement 캐시를 **먼저, 무조건** 비웁니다. 순서가 반대면 닫힌 커넥션의 핸들을 만지게 됩니다 | 17p |
| [`server/atlas/db/transaction.cpp`](server/atlas/db/transaction.cpp) | RAII 트랜잭션. 예외가 나가도 열린 채로 남지 않습니다 | 17p |
| [`server/game/equip_service.cpp`](server/game/equip_service.cpp) | **DB 제약으로 걸 수 없는 규칙**("장착 칸 하나에 아이템 하나")을 두 테이블 · 세 쓰기 한 트랜잭션으로 보장. 소유자 대조를 `WHERE` 절에 넣지 않은 이유도 여기 있습니다 | 18 – 19p |
| [`server/atlas/db/idempotency.cpp`](server/atlas/db/idempotency.cpp) | 요청 상태 기계(`None → Received → Persisted → Responded`), 커밋 전 응답을 코드가 막습니다. 라이브러리와 테스트로만 존재하고 **데모 게임에는 미연결** | 20 – 21p |
| [`server/game/character_cache.cpp`](server/game/character_cache.cpp) | 캐시는 사본이고 원본은 항상 DB. 쓰기 후 **갱신이 아니라 삭제**. Redis 미설정 · 접속 불가 · 명령 실패는 전부 캐시 미적중과 같은 경로 | 22 – 23p |

### ④ 측정과 트러블슈팅

| 파일 | 무엇을 보실 수 있는지 | 문서 |
|---|---|---|
| [`server/loadgen/load_client.cpp`](server/loadgen/load_client.cpp) | 자체 제작 부하 도구. **전송 완료 전에 송신 버퍼를 덮어쓰던 결함**을 고친 지점이 있습니다 — 서버가 아니라 이 도구가 크래시의 원인이었습니다 | 32 – 33p |
| [`docs/design/architecture-design.md`](docs/design/architecture-design.md) §16.1 | 부하 측정 전문. **버린 측정과 버린 이유까지** 실행 순서대로 남겨 두었습니다 | 25 – 29p |

문서와 코드가 어긋나면 **코드가 사실이고 문서가 버그**입니다.

---

## 지금 도는 것

| 계층 | 구현한 것 |
|---|---|
| `server/atlas/net` | Boost.Asio acceptor · session · io_runner. 접속별 순차 실행, 유휴 타임아웃, accept 백오프, `TCP_NODELAY` |
| `server/atlas/proto` | 12바이트 바이너리 프레임(`length · opcode · seq · crc32`), 필드 단위 read/write, 방향별 순번, 프레이밍 체크섬 |
| `server/atlas/db` | MariaDB Connector/C 위에 올린 자체 ORM 런타임 — 대여 타임아웃이 있는 커넥션 풀, prepared statement, RAII 트랜잭션, **대기열 상한 + 과부하 차단**을 둔 DB 전용 스레드 풀, 멱등 키와 요청 상태 기계, 커밋 이후 보상 가드 |
| `server/atlas/redis` | boost.redis 읽기 캐시 — read-through 캐릭터 캐시 + 쓰기 시 **무효화**, `ZADD`/`ZREVRANGE` 랭킹. Redis 다운이나 미설정은 캐시 미적중일 뿐 장애가 아닙니다. `PUBLISH`/`SUBSCRIBE` 래퍼는 없습니다 — 서버 간 게임 트래픽을 Redis로 흘리는 것은 설계상 금지입니다 |
| `server/atlas/config` · `core` | `server.ini`(커밋) / `.env`(시크릿) 분리, 시크릿은 **키 이름만** 로그에 남기고 값은 절대 남기지 않음. 고정폭 타입 별칭, 강타입 ID, 스레드 경계를 넘어 다니는 ctx 원장(추적 ID · 계정 · 캐릭터 · 트랜잭션 상태), spdlog 매크로, 이미지 build-id로 `file:line`을 복원하는 크래시 진단 |
| `server/game` | **GAME 바이너리.** 요청/응답 opcode 3쌍(캐릭터 로드 · 장착 · 랭킹) + 캐릭터별 락 + 장착 트랜잭션 |
| `server/generated/{info,pkt,db}` | 생성물. 손으로 고치지 않습니다. `gen:check`가 그 규칙의 기계적 강제입니다 |
| `server/console_client` · `server/loadgen` | 대화형 REPL 클라이언트, 부하 하네스(closed loop · open loop · ramp, JSONL 샘플, 실시간 TUI) |
| 배포 | 다단계 Dockerfile(`builder → runtime → symbols`), 이미지 하나 + `ATLAS_ROLE` 엔트리포인트, mysql·redis 기본 포함 `compose.yaml`(서버는 `app` 프로파일 뒤) |

**서버 권위는 실제로 동작합니다.** `shared/datas/item.csv`가 `item_id → 허용 슬롯`을 주고, 장착 경로는 클라이언트의 주장을 소유권과 슬롯 양쪽에서 거부합니다. 프레이밍 체크섬은 **위변조 방지가 아니며** 그렇게 소개하지도 않습니다. 세션 키 HMAC 계층은 **미구현**이고, 헤더에 그 자리를 예약해 두지도 않았습니다 — 0으로 채운 필드는 보호처럼 보이기 때문입니다.

---

## 실행

Windows 호스트, Docker Desktop. 명령 하나가 툴체인을 설치합니다(VS 2022 C++ 툴셋 → vcpkg 클론·부트스트랩 → 베이스라인 고정 → `npm ci` → CMake configure). 첫 실행은 Boost · OpenSSL · MySQL 클라이언트 · spdlog · GoogleTest를 소스에서 컴파일하므로 **20~60분을 잡으십시오.** 멱등합니다.

```powershell
server\setup.bat
cd server
cmake --preset windows-ci                # unity OFF 트리를 한 번 구성
cmake --build --preset windows-ci        # atlas_console 과 atlas_loadgen 도 함께 빌드
cd ..                                    # 아래는 전부 저장소 루트에서 실행
```

`windows-debug`(unity ON)는 빠른 개발 루프이고, `windows-ci`(unity OFF)는 게이트와 부하 측정이 쓰는 트리라 아래 경로는 그쪽을 가리킵니다. 둘 다 Debug 빌드입니다 — Windows Release 프리셋은 없고, 설계 문서 §16.1이 이 사실을 모든 수치의 조건으로 기록해 두었습니다.

### 1. 스택 기동

자격 증명은 `server/.env`에만 있습니다. 저장소에는 비밀번호 두 개를 뺀 `.env.example`이 들어 있습니다. `--env-file server/.env`는 **선택이 아닙니다** — compose는 `${...}` 치환을 서비스의 `env_file:`이 아니라 프로젝트 env-file에서 읽고, 필수 키는 `${VAR:?}` 형태라 하나가 비면 조용한 빈 기본값 대신 이름이 찍힌 오류가 납니다.

```powershell
cp server\.env.example server\.env      # ATLAS_DB_PASSWORD, ATLAS_DB_ROOT_PASSWORD 채우기

# 버전 스탬프. .env 에 두지 않습니다 — 빌드마다 바뀌는 값이라 파일에 두면 어제 값이 굳습니다.
$env:ATLAS_GIT_SHA    = (git rev-parse --short HEAD)
$env:ATLAS_BUILD_TIME = (Get-Date).ToUniversalTime().ToString('s') + 'Z'

docker compose --env-file server\.env --profile app build server
docker compose --env-file server\.env --profile app up -d
docker run --rm --entrypoint cat project-atlas-server /app/VERSION
```

`/app/VERSION`은 Docker 레이어 캐시가 어제 바이너리를 배포하는 것을 막는 가드이고, 같은 sha가 크래시 리포터의 build-id로 들어갑니다.

```
revision=d2c321a
built=2026-08-13T15:37:43Z
```

```powershell
docker logs project-atlas-server-1 --tail 12
```

```
[I] [crash.cpp:257] crash diagnostics ready [build=d2c321a directory=logs/crash]
[I] [secret_config.cpp:106] secrets loaded: ATLAS_DB_HOST=<set>, ATLAS_DB_PORT=<set>, ATLAS_DB_NAME=<set>, ATLAS_DB_USER=<set>, ATLAS_DB_PASSWORD=<set>, ATLAS_JWKS_URL=<empty>, ATLAS_REDIS_HOST=<set>, ATLAS_REDIS_PORT=<set>, ATLAS_REDIS_PASSWORD=<empty>
[W] [main.cpp:104] ATLAS_DB_TLS_NO_VERIFY=1 — the database connection is encrypted but the server certificate is NOT verified. Local/compose only; never production.
[I] [connection_pool.cpp:48] db connection pool ready: 4 connections
[I] [acceptor.cpp:33] acceptor listening on 0.0.0.0:7777
[I] [db_runner.cpp:32] db runner started: 2 threads, queue cap 128
[I] [handlers.cpp:232] GAME listening on 0.0.0.0:7777 (server_id=1, io_workers=16, db_threads=2)
```

로그가 자기 자신에 대해 말하는 것을 봐 주십시오 — 시크릿은 **이름만** 찍고 값은 찍지 않으며, 이 스택이 달고 있는 TLS 완화는 부팅할 때마다 경고로 스스로를 알립니다.

### 2. 캐릭터 한 행 심기

`schema.sql`은 테이블만 만들고 아무것도 넣지 않습니다 — 이 저장소에 계정 서비스가 없고, 부하 하네스는 자기 블록을 스스로 심고 지웁니다. 콘솔 데모에는 한 행이 필요합니다.

```powershell
$seed = @'
REPLACE INTO characters (server_id,character_id,account_uid,name,pos_x,pos_y,level,exp,created_at)
  VALUES (1,700001,700000,'console-demo',12,34,7,4242,NOW());
REPLACE INTO character_items (server_id,character_id,item_uid,item_id,stack_count,equip_slot) VALUES
  (1,700001,7000011,1001,1,1),
  (1,700001,7000012,1002,1,0),
  (1,700001,7000013,2001,1,2);
'@
$seed | docker exec -i project-atlas-mysql-1 sh -c 'mysql -uroot -p"$MYSQL_ROOT_PASSWORD" "$MYSQL_DATABASE"'
```

아이템 ID는 `shared/datas/item.csv`에서 옵니다 — `1001`/`1002`는 무기(슬롯 1), `2001`은 방어구(슬롯 2)입니다.

### 3. 직접 태워 보기

```powershell
server\build\windows-ci\console_client\atlas_console.exe --host 127.0.0.1 --port 7777
```

모든 명령이 실제 소켓 위의 실제 프레임입니다. 클라이언트가 응답을 지어내는 명령은 하나도 없습니다. 실제 세션 — 무기를 장착하고(한 트랜잭션 안에서 세 번 쓰기), 방어구를 무기 슬롯에 넣어 보는 과정입니다.

```
connected to 127.0.0.1:7777
atlas> load 700001
character 700001 account=700000 name=console-demo level=7 exp=4242 pos=(12,34)
inventory (3 items)
  uid=7000011 item_id=1001 stack=1 slot=1(weapon) csv_slot=1
  uid=7000012 item_id=1002 stack=1 slot=0(none) csv_slot=1
  uid=7000013 item_id=2001 stack=1 slot=2(armor) csv_slot=2
atlas> equip 7000012 1
equip ok: uid=7000012 -> slot 1 (uid=7000011 was unequipped)
atlas> equip 7000013 1
equip refused: item does not go in that slot (uid=7000013, slot 1) — connection stays up
atlas> rank 5
ranking (1 entries, highest exp first)
  #1 character=700001 exp=4242
atlas> quit
```

**거부가 핵심입니다.** 슬롯 규칙은 서버에 있고, 클라이언트는 요청을 거르지 않고 그대로 보내며, 거부된 요청은 연결을 끊지 않습니다. `rank`는 Redis 정렬 집합에서 답하는데, 이 집합은 `Commit()` 다음 줄에서 갱신되고 그 전에는 절대 갱신되지 않습니다.

### 4. 부하 걸기

하네스는 DB에 직접 붙어 자기 캐릭터 블록을 심으므로 서버와 같은 자격 증명이 필요합니다. `server\.env`를 셸에 올리고 호스트만 덮어쓰십시오 — `.env`의 값은 compose 서비스 이름이라 네트워크 밖에서는 해석되지 않습니다.

```powershell
Get-Content server\.env -TotalCount 200 | Where-Object { $_ -match '^\s*[A-Z]' } |
    ForEach-Object { $k,$v = $_ -split '=',2; Set-Item "env:$k" $v }
$env:ATLAS_DB_HOST = '127.0.0.1'

server\build\windows-ci\loadgen\atlas_loadgen.exe --connections 64 --duration 45 --warmup 15 `
    --io-threads 8 --host 127.0.0.1 --port 7777 --server-id 1 --sample-jsonl reports\run.jsonl
node tools\loadreport\loadreport.js --in reports\run.jsonl
```

```
seeded 64 characters (server_id=1, character_id 900000..900063)
established=64 peak_live=64 connect_fail=0 transport_fail=0 load_fail=0
requests_sent=10255 ok=10255 unavailable=0 refused=0 load_rejected=0
steady_window_ms=30000 samples=4732 throughput_rps=157.7
p50_us=476884 p90_us=536277 p99_us=591742 max_us=600765
seeded rows removed
```

리포트는 `npm run loadreport -- …`가 아니라 `node tools\loadreport\loadreport.js`로 실행합니다 — npm의 PowerShell 셈이 `--` 구분자를 삼켜서 스크립트가 `--in` 대신 맨 경로를 받습니다. 심어 둔 블록은 종료 시 MySQL과 Redis 양쪽에서 삭제됩니다.

`--ramp 32,64,128,192,256 --stage-seconds 20`을 주면 단계별 버전이 돕니다. 여기서 나온 수치를 믿기 전에 [설계 문서 §16.1](docs/design/architecture-design.md)을 먼저 읽어 주십시오 — **이 호스트에는 디스크 상태가 두 가지 있고, 그 경계를 넘나든 측정은 둘 중 어느 쪽도 측정한 것이 아닙니다.**

---

## 검증

```powershell
powershell -NoProfile -File server\scripts\ci-gate.ps1
```

| 단계 | 무엇을 증명하는지 |
|---|---|
| `gen:check` | 생성물이 여전히 입력과 일치함. "생성물을 손으로 고치지 않는다"의 기계적 강제입니다 — 문서 규칙만으로는 결국 손으로 고친 생성물이 커밋됩니다 |
| `core-purity` | `server/atlas/**`가 게임 계약 헤더를 include 하지 않고 데모 게임 어휘를 담지 않음. "코어는 게임을 몰라야 한다"의 기계적 강제 |
| `format-check` | `clang-format` |
| `clang-tidy` | **로컬에서는 이유를 출력하고 건너뜁니다.** Ninja/`cl` 트리가 내놓는 MSVC 프리컴파일 헤더를 clang-tidy가 읽지 못해, 이 단계는 Linux CI에서만 채점됩니다 |
| `build` (unity ON) | 빠른 개발 구성 |
| `build` (unity OFF) | 누락된 `#include`와 ODR 충돌. unity 빌드는 같은 배치의 형제 파일이 헤더를 대신 공급해 이 둘을 가립니다 — 이 단계는 절대 건너뛰지 않습니다 |
| `test` | `ctest`로 GoogleTest 실행. 게이트가 `server/.env`를 로드하고, **DB에 접속 가능한데도 테스트가 건너뛰어졌으면 실패 처리**합니다 — `ctest`는 skip에도 0을 반환하므로, 이 규칙이 없을 때 아무것도 증명하지 않은 채 PASS를 찍고 있었습니다 |

최근 로컬 실행: **140/140 통과, 0 skip, `[gate] PASS`.**

**두 게이트는 양방향으로 서로를 대체하지 못합니다.**

- **CI에는 MySQL 서비스가 없습니다.** 모든 DB · Redis 스위트가 거기서 건너뛰므로, 영속성 축은 접속 가능한 DB를 상대로 한 **로컬** 게이트 실행이 증명합니다 — 위의 "skip은 실패" 규칙이 그래서 있습니다. CI 배지가 초록이어도 ORM에 대해서는 아무 말도 하지 않습니다.
- **로컬 게이트는 `clang-tidy`를 돌리지 않습니다.** 네이밍 · `bugprone` · `modernize` 위반은 CI에서만 드러납니다. `server\scripts\tidy-prefilter.ps1`이 PCH를 걷어낸 `compile_commands.json` 사본 위에서 도는 로컬 사전 필터이긴 하지만, 의도적으로 **게이트가 아닙니다** — 항상 0으로 종료하고, 두 툴체인은 양방향으로 어긋납니다. CI의 libstdc++는 MSVC STL이 보고하지 않는 optional 접근을 잡고, CI는 이 스윕만 채점할 수 있는 `#if defined(_WIN32)` 분기를 아예 컴파일하지 않습니다.
- 양쪽에서 도는 `format-check`조차 어긋날 수 있습니다 — 로컬 `clang-format`은 VS에 딸려 오고 CI 것은 `apt.llvm.org`에서 옵니다.

초록 로컬 게이트는 초록 CI의 증거가 아니고, 그 역도 아닙니다.

---

## 부하 측정

아래 수치는 [설계 문서 §16.1](docs/design/architecture-design.md)에서 인용했습니다. **측정 조건은 결과의 일부이며 결과와 분리되지 않습니다.**

**조건.** i5-12600KF(10코어 / 16논리) · 32 GB · Windows 11. 서버는 Docker Desktop / WSL2(16 vCPU) 컨테이너, 이미지는 `linux-release` 프리셋(최적화) 빌드. MySQL 8.4.10은 같은 호스트의 컨테이너에서 이미지 기본값(`innodb_flush_log_at_trx_commit=1`, `sync_binlog=1`, `log_bin=ON`, `innodb_flush_method=O_DIRECT`). 서버 런타임은 `io_workers=16`, `db_threads=2`, `db_pool_size=4`, DB 대기열 상한 128. **부하 클라이언트는 Debug · 비최적화 빌드**(`windows-ci`가 유일한 Windows 프리셋이고 Release 프리셋이 없습니다)이며, **서버 · DB와 같은 호스트에서** 루프백으로 돕니다. 모든 표는 독립 실행 3회 이상의 중앙값과 관측된 min–max이고, 실행 전후로 호스트 `fsync` 프로브를 뜹니다. 중간에 상태 경계를 넘은 실행은 버렸고, 버렸다는 사실도 기록했습니다.

**① 이 호스트에는 디스크 상태가 둘 있고, 그 사실이 나머지 전부를 압도합니다.**

| 상태 | fsync 프로브 | 64접속 closed-loop 상한 | 진입 조건 |
|---|---:|---:|---|
| 포화 | 3.7 – 4.9 ms | **129.6 – 142.3 req/s** (중앙값 139.5, 9회) | 포화 부하 35~40초 지속 후 |
| 회복 | 1.0 – 1.5 ms | **357 – 463 req/s** (중앙값 445, 6회) | 수 분 유휴, 또는 용량 이하 지속 |

프로브 비율 3.7배, 처리량 비율 3.3배 — 같은 크기입니다. 서버는 바뀌지 않았고, 커밋 하나가 디스크를 기다리는 시간이 바뀌었습니다. **정직한 숫자는 포화 쪽**입니다. 회복 상태는 실제 부하 1분 안에 소진되기 때문입니다. 아래는 전부 포화 상태 기준입니다.

**② 처리량은 동시성에 반응하지 않고, 지연은 순수한 대기입니다.**

| 동시 접속 | 처리량(중앙값) | p50 | p99 | `N ÷ 처리량` |
|---:|---:|---:|---:|---:|
| 1 | 92.7 req/s | 10.2 ms | 17.5 ms | (미포화) |
| 2 | 127.8 req/s | 15.1 ms | 24.2 ms | 15.6 ms |
| 8 | 129.5 req/s | 60.4 ms | 79.0 ms | 61.8 ms |
| 64 | 139.5 req/s | 454 ms | 581 ms | 459 ms |
| 256 | 134.5 req/s | 1883 ms | 2214 ms | 1903 ms |

동시성 64배, 처리량 ±2 %, 지연 63배. Little's Law가 전 구간에서 **2.1 %** 오차로 닫힙니다 — 무릎은 동시성 **2**에 있고, 그 뒤는 전부 대기열입니다.

**③ 병목을 근거와 함께 지목합니다.** CPU가 아니고(요청측 CPU는 두 상태에서 동일하며 16코어 중 0.4코어), 락 경합이 아니고(`Innodb_row_lock_waits = 0`, 54회 전부), InnoDB 스톨도 아닙니다(`Innodb_log_waits = 0`, 더티 페이지 평탄). **`db_threads = 2` × 커밋당 fsync 2.1회 × 그 시점의 fsync 지연**입니다. `redo`와 `binlog`가 각각 sync 하고, MySQL은 동시 트랜잭션을 2개 넘게 본 적이 없어 group commit이 묶을 것이 없습니다. **튜닝 라운드는 돌리지 않았고 주장하지도 않습니다** — 이것은 진단이지 최적화가 아닙니다.

**④ 고정 레이트에서 유저가 겪는 것**(open loop, 64접속):

| 목표 | 달성 | p50 |
|---:|---:|---:|
| 110 req/s (용량의 83 %) | 110.1 | 13.1 ms |
| 130 req/s (용량의 98 %) | 130.0 | 22.7 ms |

**⑤ 상한을 넘기면 대기열 상한이 지연 발산을 거절로 바꿉니다**(ramp, 4회):

| 단계 | 접속 | 유효 처리량 | 거절률 | 수용분 p50 |
|---:|---:|---:|---:|---:|
| 2 | 128 | 129.5 req/s | 0.00 % | 972.9 ms |
| 3 | 192 | 114.9 req/s | **99.28 %** | 1111 ms |
| 4 | 256 | 111.0 req/s | **99.43 %** | 1145 ms |

상한 위에서는 수용분 p50이 `N`을 따라가기를 멈추고 `대기열 상한 ÷ 처리량`에 고정됩니다(오차 0.3 %, 0.7 %) — 대기열에 뚜껑이 덮인 것입니다. **과부하 차단은 공짜가 아닙니다**: 유효 처리량이 14 % 떨어졌습니다. 거절도 프레임 인코딩과 소켓 쓰기 비용을 쓰고, 이 하네스에는 백오프가 없어 거절마다 즉시 다음 요청으로 답하기 때문입니다. 클라이언트가 센 거절 수와 서버가 센 거절 수는 **4회 전부 정확히 일치**했습니다(불일치 0건 · 676,875 · 646,641 · 613,163 · 674,276).

---

## 코드 생성 — 게임 교체 이음매

패킷 · DB 접근 · 정적 데이터를 생성합니다. 같은 코어에 다른 게임을 얹을 수 있게 하는 지점입니다. 저장소 루트에서:

```powershell
npm run gen:all      # info → db → pkt: 기획 데이터, 그다음 스키마, 그다음 패킷
npm run gen:check    # 드리프트 게이트. CI 게이트의 첫 단계입니다
```

`server/generated/` 아래는 손으로 고치지 않습니다. `shared/contracts/*.cs`, `shared/datas/*.csv`, `server/db/schema.json`을 고치고 생성기를 다시 돌리십시오.

**아직 게임 1종에만 적용했습니다.** 2종을 붙여 봐야 재사용 가능하다고 말할 수 있습니다.

---

## 아직 없는 것

위의 어느 것도 할인해서 읽지 않으셔도 되도록, 여기에 이름을 붙여 둡니다.

| 미구현 | 상태 |
|---|---|
| **FE · WORLD 바이너리** | 설계 완료(토폴로지 · 레지스트리 · 하트비트 · 동적 attach/detach). 엔트리포인트는 `ATLAS_ROLE=fe`/`world`를 존재하지 않는 바이너리로 매핑하고, 있는 척하는 대신 69로 종료합니다 |
| **Actor / AoI / 행동 트리 월드 루프** | 설계만 했습니다. 이 저장소에 틱 루프도, 공간 모델도, BT 엔진도 없습니다. 이동과 채팅은 핸들러 없는 생성 패킷 계약으로만 존재합니다 |
| **Relay / Matching / InterWorld** | Phase 3. 이음매만 고정해 두었습니다 — WORLD ↔ WORLD 직접 통신 금지, 그 우회로인 Redis pub/sub도 금지 |
| **세션 키 HMAC(무결성 2계층)** | 세션 키가 필요하고, 그건 인증 핸드셰이크가 필요합니다. 프레임 헤더에 예약 필드를 두지 않았습니다 — 0으로 채운 필드는 보호처럼 보이기 때문입니다 |
| **JWKS / platform-auth 연동** | 설계 완료. `ATLAS_JWKS_URL`은 읽기만 하고 쓰지 않습니다 |
| **서버 치트 콘솔 · 패킷 로그** | 설계 완료, 미구현 |
| **멱등성 저장소 영속화** | 인메모리입니다. 프로덕션이라면 테이블이 들어갈 자리인데, 데모 테이블 예산을 이미 다 썼습니다 |
| **CD** | Phase 1 범위 밖으로 결정. 배포는 단일 호스트의 `compose`에서 끝납니다 |

---

## 문서

`docs/` 아래는 전부 **한국어**입니다. `AGENTS.md`(영문)가 라우팅 인덱스입니다.

| 문서 | 내용 |
|---|---|
| [`docs/design/architecture-design.md`](docs/design/architecture-design.md) | 아키텍처 SoT — 범위 · 토폴로지 · 식별자 · 프로토콜 · 스레드 모델 · ORM과 캐시 · 로깅/예외 정책 · 빌드 체인, 그리고 §16.1 부하 측정 전문(버린 실행과 버린 이유 포함) |
| [`docs/conventions/cpp-style.md`](docs/conventions/cpp-style.md) | 코딩 컨벤션 SoT와 3계층 기계적 강제 |
| [`AGENTS.md`](AGENTS.md) | 저장소 지도, 강제 규칙, 어떤 변경이 어떤 문서를 먼저 읽어야 하는지 |
| [`README.en.md`](README.en.md) | 이 문서의 영문판 |

---

## 라이선스

[MIT](LICENSE).
