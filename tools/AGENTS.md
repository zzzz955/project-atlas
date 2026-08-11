# tools/ — 데이터 파이프라인 (Node)

게임을 갈아끼우는 **seam**이다(`docs/design/architecture-design.md §14`). 코어 C++ 는 고정이고,
게임별로 바뀌는 계약 · 스키마 · 정적 데이터는 전부 여기서 **생성**된다.

## 입력 → 출력

| 생성기 | 입력 | 출력 | 상태 |
|---|---|---|---|
| `pkt_generator` | `shared/contracts/**/*.cs` | `server/generated/pkt/` (C++ 패킷 · DTO · 필드별 write/read) | 구현 완료 (2026-08-06, wp6). 상세는 `pkt_generator/AGENTS.md` |
| `db_generator` | `server/db/schema.json` | `server/generated/db/` (POD row 구조체 · `ColumnMeta` 배열 · `?` placeholder SQL 상수 · 바인딩 순서 배열) + `server/generated/db/schema.sql` | 구현 완료 (2026-08-06, wp7). 🔴 **CRUD 실행 API는 생성하지 않는다** — 커넥션 · 트랜잭션 · per-character lock 은 ORM 런타임 노드 몫(`design §10.1`). 상세는 `db_generator/AGENTS.md` |
| `info_generator` | `shared/datas/**/*.csv` (5행: 필드명 / 타깃 `C`\|`S`\|`CS` / 타입 / 제약 / 데이터) | `server/generated/info/` (컴파일-인 정적 테이블 · `InfoColumnMeta` 배열 · PK 이분 탐색 + 집합 헤더 `info_all.h`) | 구현 완료 (2026-08-11, wp11). 🔴 **DB 테이블이 아니다** — 행이 바이너리에 박히고 런타임 로드가 없다. 소비처는 `server/game/equip_service.cpp`(`design §8.2` 3층 서버 권위). 상세는 `info_generator/AGENTS.md` |

공통 기반은 이 디렉터리에 있다.

| 파일 | 역할 |
|---|---|
| `config-loader.js` | `template.ini` + `types.json` 로드 → `paths` / `dataGen` / `dbGen` / `pktGen` / `types`. 파싱 실패 시 `[config] ERROR:` 출력 후 exit 1 |
| `types.json` | **정규화 타입 → 엔진 타입 매핑의 SoT.** `cpp` · `mysql` · `csharp` · `gdscript` 4열 |
| `all_generator.bat` | 일괄 실행 배치. 순서 `info → db → pkt`, 단계 실패 시 중단, `tools/logs/` 에 타임스탬프 로그. `GEN_BATCH_NO_PAUSE=1` 이면 종료 대기 없음 |

## `core_purity/` — 🔴 생성기가 아니라 검사기다

`tools/` 에 있지만 위의 입력 → 출력 표에 넣지 않는다. **아무것도 생성하지 않는다** — 스캔하고 exit
code 로만 말한다. `architecture-design.md §15.4` 게이트의 두 번째 단계이며, `§14` 의
"코어(고정) / 계약(생성) / 게임(교체)" 경계를 기계로 강제한다. `gen:check` 가 "생성물 손편집"에 대해
하는 일을, 이 검사기가 "코어 오염"에 대해 한다.

| 파일 | 역할 |
|---|---|
| `core_purity.js` | `server/atlas/**/*.{h,cpp}` 스캔. `--root <dir>` 로 루트 교체(픽스처용). 🔴 스캔 파일 0개면 exit 1 — 경로 오타로 영원히 초록불이 되는 것이 이 검사의 최악 실패 모드다 |
| `denylist.txt` | 규칙 2 용어 목록(§3.3 데모 게임 어휘). 대소문자 무시 **부분 문자열** 매칭 — `boss_id` · `ApplySkillDamage` 를 잡기 위해서다. 🔴 데모 게임이 자라면 이 목록도 같이 자란다 |
| `allowlist.txt` | 규칙 1 예외. 오늘 항목 0개. 각 줄은 `<경로>  # <사유> (§절번호)` 이며 🔴 사유 없는 줄은 검사기가 exit 1 한다 |
| `testdata/` | 위반 픽스처 2개(`rule1_violation` · `rule2_violation`). 검사기가 실제로 잡는지 확인용이며 컴파일 대상이 아니다 |

검사 2가지 — ① `server/atlas/**` 가 `generated/` 헤더를 include 하지 않는다(코어는 opcode 로
디스패치하지 게임 DTO 를 알지 않는다) ② `denylist.txt` 용어가 0건. 제외: `generated/`(생성물) ·
`tests/`(테스트는 게임 어휘를 써도 된다). 주석과 코드를 구분하지 않는다 — 주석에 게임 이름이 박히는
것도 오염이다.

🔴 구현이 Node 한 벌인 이유: 로컬 게이트는 PowerShell(`server/scripts/ci-gate.ps1`), CI 는
bash(`.github/workflows/ci.yml`) 인데 스크립트를 두 벌 쓰면 같은 규칙의 두 전사본이 조용히 갈라진다.
`gen:check` 가 이미 쓰는 "양쪽에서 `npm run` 으로 부른다" 패턴을 그대로 따른다.

## 실행

레포 **루트**에서 실행한다(`config-loader` 가 루트 기준으로 `template.ini` 를 찾는다).

```
npm run gen:all      # info → db → pkt 전체 재생성 (입력 데이터가 스키마·패킷보다 앞선다)
npm run gen:info     # 개별
npm run gen:db
npm run gen:pkt
npm run gen:check    # 드리프트 게이트 — 생성물이 입력과 어긋나면 exit 1

npm run gen:db:apply # 🔴 dev 전용. 마이그레이션 SQL 을 실제 DB 에 적용한다. ATLAS_ENV=dev 가
                     #    아니면 소켓을 열기 전에 exit 1 (AD §10.7). prod 는 사람이 직접 적용
npm run check:core-purity   # 코어 순수성 게이트 — 코어가 게임을 알면 exit 1 (생성기 아님)
```
🔴 라이브 DB 를 읽는 경로(`gen:db` 의 diff, `gen:db:apply`)만 `mysql2` 를 **지연 `require`** 한다.
`devDependencies` 에 선언돼 있으므로 `npm ci` 하나면 갖춰진다 — 🔴 **선언하지 않고 "각자 설치"로
두면 안 된다.** 그 경우 새 클론에서 `gen:db` 가 "DB 도달 불가"와 구분되지 않는 경고만 남기고
**exit 0** 으로 끝나 마이그레이션이 조용히 생성되지 않는다. `gen:check` 는 이 경로를 타지 않으므로
**DB 도 `mysql2` 도 없이 여전히 exit 0** 이다(오프라인 요구 유지).
`tools\all_generator.bat` 은 `gen:all` 과 같은 순서를 배치로 돌리며 로그 파일을 남긴다.
🔴 `gen:*` 스크립트는 **이 레포가 실제로 만드는 생성기**만 가리킨다. 아무도 쓰지 않을 생성기를 가리키는
스크립트를 남겨 두면 `gen:check` 가 영구히 exit 1 이 되어 드리프트 게이트가 무의미해진다.
현재 `gen:check` 는 exit 0 이다(info 6개 · db 6개 · pkt 10개, `changed=0`). 위험한 것은 **영원히
초록불이 될 수 없는** 조성 — 존재하지 않는 생성기를 가리키는 스크립트다. 🔴 생성기 3종이 모두
착지했으므로(2026-08-11) 이제 이 규칙이 막는 것은 **네 번째** 생성기를 미리 배선하는 일이다
(`stack_generator` 승격 조건은 `design §14`).

## 🔴 규칙

- **생성 출력을 직접 편집하지 않는다.** `server/generated/**` 는 전부 생성물이다. 고칠 것이 있으면
  입력(`shared/contracts` · `server/db/schema.json` · `shared/datas`)이나 생성기를 고치고 다시 돌린다.
  수동 편집은 다음 `gen:all` 에서 조용히 사라지고, `gen:check` 가 CI 에서 잡는다.
- **타입은 `types.json` 의 정규화 타입만 쓴다.** 새 타입이 필요하면 `types.json` 에 4열을 모두 채워
  추가한다. `cpp` 열이 비면 `config-loader` 가 로드를 거부한다.
- 🔴 **`cpp` 열에 `int` / `long` 을 쓰지 않는다** — `cpp-style.md §4.1`. 고정폭 별칭(`Int32`, `UInt64`,
  `Float32`)만 쓴다. Windows LLP64 / Linux LP64 차이로 직렬화 결과가 갈린다.
- 🔴 **`template.ini` 에 시크릿을 넣지 않는다.** 경로 · 타깃 · 네임스페이스만 둔다. 접속 정보 · 키는
  `.env` 이며 커밋되는 것은 `*.example` 뿐이다.
- `bool` 의 **직렬화 폭**은 이 매핑이 정하지 않는다 — `pkt_generator` 의 emitter 책임이다
  (`cpp-style.md §4.4` enum/폭 규칙).
