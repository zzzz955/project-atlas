# db_generator — 스키마(JSON) → MySQL DDL + C++ 행 구조체 / prepared SQL

스키마 한 벌에서 여러 타깃을 뽑는 것이 이 프레임워크의 seam 이다
(`docs/design/architecture-design.md §14`). 이 디렉터리는 그 중 **SQL 타깃과 C++ 타깃**을 소유한다.

> 루트 `tools/AGENTS.md` 는 파이프라인 전체를 설명한다 — 이 문서는 db 타깃의 세부다.

## 입력 → 출력

실행은 레포 **루트**에서: `npm run gen:db` / 드리프트 검사 `npm run gen:db:check` /
dev DB 적용 `npm run gen:db:apply`(→ [라이브 DB 동기화](#라이브-db-동기화--dev-전용)).
경로 · 네임스페이스는 전부 `template.ini [db-gen]` 이 소유한다 — 하드코딩된 게임/엔진 경로는 없다.
**접속·환경 키는 ini 에 0개**이며 `.env` 에서 lazy 로 읽는다(`design §5.4` · `§10.7`).

| ini 키 | 값 | 의미 |
|---|---|---|
| `schema` | `server/db/schema.json` | 입력 |
| `sql_output` | `server/generated/db/schema.sql` | MySQL DDL 출력 |
| `cpp_db_output_dir` | `server/generated/db` | C++ 출력 |
| `cpp_namespace` | `atlas::generated` | 생성 네임스페이스 |

### 출력 파일

| 파일 | 내용 |
|---|---|
| `schema.sql` | 테이블별 `CREATE TABLE IF NOT EXISTS` (PK · UNIQUE · FK · 인덱스 · 기본값). **오프라인 전체 생성 스크립트**이며 마이그레이션이 아니다 — 증분 변경은 `server/db/migrations/` 로 간다 |
| `db_meta.h` | `enum class ColumnType : UInt8` (열거자는 `types.json` 정규화 타입 + `DateTime`/`Json`) · `struct ColumnMeta` · `constexpr CountPlaceholders()` |
| `db_meta.cpp` | 헤더를 자기 완결적으로 만드는 TU 하나. unity-OFF 빌드에서 include 누락을 잡는 게이트다 |
| `<table>_row.h` | 테이블 하나당: 행 구조체 · 폭 `static_assert` · 컬럼 인덱스 상수 · `std::array<ColumnMeta, N>` · `string(N)` 길이 상수 · prepared SQL 상수 4종 + 바인딩 배열 + 대응 `static_assert` |
| `<table>_row.cpp` | 위와 같은 자기 완결성 TU |
| `db_all.h` | 집합 헤더 — `db_meta.h` + 전체 `<table>_row.h`. 목록도 생성물이다: "행 구조체 전부가 한 include 로 온다"는 주장은 테이블이 추가되는 순간 썩는다. `info_all.h` 와 같은 모양 |
| `db_sources.cmake` | `set(ATLAS_GENERATED_DB_SOURCES ...)`. 소스 목록도 생성물이다 — 테이블을 추가해도 CMake 를 손대지 않고, 낡은 목록이 구조체를 조용히 빠뜨릴 수 없다 |
| `server/db/migrations/{ts}_schema_sync.sql` | **DB 에 닿았을 때만** 나오는 diff 결과. dev→prod 인수인계 매체이므로 **커밋한다**. 아래 절 참조 |

`server/generated/db/CMakeLists.txt` 와 `tests/` 만 손으로 쓴 것이고, 나머지는 전부 생성물이다.

### 입력(`schema.json` 컬럼) → 출력 매핑

| 스키마 표기 | C++ 필드 (`<table>_row.h`) | `ColumnType` | MySQL (`schema.sql`) |
|---|---|---|---|
| `"type": "uint16"` 등 정규화 타입 | `types.json` 의 `cpp` 열 (`UInt16` …) | `UInt16` … | `types.json` 의 `mysql` 열 |
| `"type": "string(N)"` | `std::string` + `k<Table><Column>MaxLength = N` | `String` | `VARCHAR(N)` |
| `"type": "datetime"` | `SysTime` (`atlas/core/time.h` — 벽시계) | `DateTime` | `DATETIME` |
| `"type": "json"` | `std::string` | `Json` | `JSON` |
| 이름이 강타입 ID 표에 있음 | `CharacterId` · `AccountId` · `SessionId` · `ActorId` | 저장 폭 그대로 `UInt64` | `BIGINT UNSIGNED` |
| `"null": true` (비-PK) | `std::optional<T>` | `nullable_ = true` | `NULL` |
| `"pk": true` | 위와 동일 (PK 는 NOT NULL 을 함의) | `primary_key_ = true` | `PRIMARY KEY (…)` — 복합 PK 지원 |
| `"default": v` (비-nullable) | 멤버 초기자 `{v}` | — | `DEFAULT v` |

**강타입 ID 표는 emitter 안에 명시적으로 있다**(`ID_COLUMNS`, `cpp-style.md §4.3` · `server/atlas/core/ids.h`):
`account_id` · `account_uid` → `AccountId`, `character_id` → `CharacterId`, `session_id` → `SessionId`,
`actor_id` → `ActorId`.
**`server_id` 는 일부러 표에 없다** — `ids.h` 가 "인자로 돌아다니지 않는 값에 강타입을 주면 얻는
것이 없다"는 이유로 아직 약타입으로 두었고, 이 표가 그 결정을 앞질러 가지 않는다.
`account_uid` 는 platform-auth 가 소유하는 계정 식별자(`design §6`)이며 이름만 uid 일 뿐 같은 계정
아이덴티티라 `AccountId` 로 뽑는다. `ColumnMeta` 는 **DB 컬럼**을 서술하므로 ID 컬럼도 `UInt64`
그대로다 — C++ 필드 타입과 다른 것이 정상이다.

## prepared statement

테이블당 4종. **전부 `?` placeholder 가 박힌 고정 문자열 상수**이고, 컬럼명·값을 런타임에 이어
붙이는 코드는 생성하지 않는다(`design §10` — 문자열 SQL 조립 금지).

| 상수 | SQL | 바인딩 순서 |
|---|---|---|
| `k<Table>SelectByPkSql` | `SELECT <모든 컬럼> FROM t WHERE <pk> = ? …` | PK |
| `k<Table>InsertSql` | `INSERT INTO t (<모든 컬럼>) VALUES (?, …)` | 선언 순서 전체 |
| `k<Table>UpdateByPkSql` | `UPDATE t SET <비-PK> = ? … WHERE <pk> = ? …` | 비-PK 먼저, **PK 가 마지막** |
| `k<Table>DeleteByPkSql` | `DELETE FROM t WHERE <pk> = ? …` | PK |

바인딩 배열은 `std::array<std::size_t, N>` 의 **컬럼 인덱스**이며 `k<Table>Col<Column>` 상수로
쓴다. 각 쌍마다
`static_assert(CountPlaceholders(...Sql) == ...Binding.size())` 가 붙는다 — "SQL 과 바인딩이 맞는다"가
주석의 약속이 아니라 **빌드 실패**로 강제된다.

## 이 생성기가 만들지 않는 것

**ORM 런타임 전부** — 커넥션 · 트랜잭션 스코프 · per-character lock · CRUD 실행 API
(`design §10.1` 의 경계표). 런타임이 없는데 실행 코드를 뽑으면 컴파일되지 않는다. 런타임 노드가
붙으면 이 emitter 를 확장해 CRUD 함수 시그니처까지 뽑고, 그 함수들이 위 SQL 상수와 바인딩 배열을
그대로 쓴다.

**EF Core 엔티티 / DbContext 타깃**도 이 레포에는 소비자가 없어 가져오지 않았다(`template.ini` 에
해당 키가 없는 것이 그 사실을 못 박는다).

**금지(Phase 1, `design §10`)**: 관계 자동 로딩 · lazy loading · change tracking / 유닛 오브 워크 ·
**prod 마이그레이션 자동화**. ORM 은 범위가 폭발하는 대표 항목이다 — 위 목록에 없는 기능을
추가하지 않는다.

## 라이브 DB 동기화 — dev 전용

`design §10.7` 이 SoT 다. 여기에 근거를 복제하지 않고 **동작만** 적는다.

| 명령 | 하는 일 | DB 변경 |
|---|---|---|
| `npm run gen:db` | 위 생성물 + (DB 에 닿으면) diff → `server/db/migrations/{ts}_schema_sync.sql` | **없다** — `INFORMATION_SCHEMA` `SELECT` 뿐 |
| `npm run gen:db:apply` | 그 마이그레이션을 실행하고 `schema_migrations` 원장에 기록 | 있다. `ATLAS_ENV=dev` 필수 |
| `npm run gen:db:apply -- --allow-drops` | 위 + `MODIFY` / `DROP` 까지 실행 | 있다. 데이터 손실 경로 |
| `npm run gen:db:check` | 오프라인 드리프트 검사 | **접속 자체가 없다** |

- **`gen:db:apply` 는 `gen:all` · `gen:check` 조성에 없다.** 생성물을 갱신하려던 사람이 스키마를
  적용하게 되면 안 된다.
- **`ATLAS_ENV != dev`(미설정 포함)면 소켓을 열기 전에 exit 1.** prod 는 사람이 SQL 파일을 읽고
  적용한다 — 규약이 아니라 이 검사가 강제한다.
- **파괴적 변경(`MODIFY` · `DROP`)은 파일에 `-- !destructive:` 표식이 붙은 주석으로 나온다.**
  파일을 그대로 실행하면 건너뛴다. `--allow-drops` 만 표식을 떼고 실행한다.
- **DB 미도달 · 드라이버 미설치 → diff 단계만 skip, exit 0.** `gen:check` 가 `design §15.4`
  게이트의 선두이고 CI 에는 MySQL 이 없다 — 여기서 접속을 시도하면 드리프트 게이트가 통째로 죽는다.
  (`--apply` 에서는 같은 상황이 exit 1 이다. 적용이 조용히 성공한 척하면 안 된다.)
- **`mysql2` 는 이 레포의 의존성이 아니다.** 필요할 때만 `npm install mysql2 --no-save`.
  없으면 위 규칙대로 diff 만 건너뛴다.
- 접속 정보는 `server/.env` 에서 **lazy** 로 읽고, **키 이름과 출처만 로그에 찍는다 — 값은 절대
  찍지 않는다**(`design §5.4`).
- 원장(`schema_migrations`)은 **diff 대상에서 제외**된다. 자기 자신을 마이그레이션하려 들면
  매 실행마다 자신을 DROP 하려 한다.
- 직전 마이그레이션과 본문이 같으면 새 파일을 만들지 않고 재사용한다.

## 생성기가 거부하는 것 (조용히 통과시키지 않는다)

| 거부 대상 | 이유 |
|---|---|
| PK 없는 테이블 | by-PK 문 4종이 PK 에서 나오고, per-character lock 단위도 PK 다(`design §6`·`§10`) |
| 강타입 ID 컬럼의 폭 불일치 (예: `character_id: uint32`) | `CharacterId` 는 `UInt64` 기반(`ids.h`). 좁게 저장하면 서버가 발급한 식별자가 조용히 잘리고, 운영 데이터에 가서야 드러난다 |
| 매핑 없는 타입 | 타입 SoT 는 `tools/types.json` 이다. 4열을 모두 채워 추가하거나 컬럼을 고친다 |
| `mysql` 열이 빈 타입 | `CREATE TABLE` 을 만들 수 없다 |
| 중복 테이블 · 중복 컬럼 · 식별자 규칙 위반(`^[a-z][a-z0-9_]*$`) | — |
| 존재하지 않는 컬럼을 가리키는 인덱스 | — |
| `tables` 가 비었거나 `database` 가 없음 | 빈 출력 디렉터리는 모든 행 구조체를 빌드에서 조용히 떨어뜨린다 |

거부는 **비제로 exit + `[db] ERROR:` 로 시작하는 다중 행 메시지**다. 검증을 전부 끝낸 뒤에
파일을 쓴다 — 뒤쪽 테이블이 거부당했는데 앞쪽 구조체만 새로 쓰인 반쪽 출력이 남으면 안 된다.

## 테스트

| 위치 | 무엇 |
|---|---|
| `server/generated/db/tests/db_schema_test.cpp` | ① **스키마 ↔ 구조체 대조** — `schema.json` 을 손으로 옮겨 적은 컬럼 표를 생성 메타데이터와 컬럼 단위로 비교(생성기로 만든 기대값은 자기 자신과의 일치만 증명한다) ② 폭 `static_assert` ③ `?` 개수 == 바인딩 길이 · UPDATE 는 PK 를 마지막에 바인딩 · SELECT 투영 순서 == 메타데이터 순서 · 문 안에 따옴표 리터럴 0건 ④ `character_id` 가 `CharacterId` 이고 `UInt64` 와 양방향 암묵 변환이 없음 |
| `server/generated/db/tests/CMakeLists.txt` | 위 + `db.generator_rejects_narrow_id_{exit,message}` — `testdata/id_width_mismatch/` 를 `--schema` 로 물려 **생성기 자체**를 테스트한다 |

`testdata/` 는 `server/db/` 밖에 있으므로 평소 `npm run gen:db` 는 이 픽스처를 보지 않고, 출력도
빌드 트리로 빠져 `server/generated/db/` 를 건드리지 않는다.

`ctest --preset windows-ci -R db --output-on-failure`

## CLI

| 플래그 | 용도 |
|---|---|
| `--check` | 드리프트 검사만. 파일을 쓰지 않고, **DB 에 접속하지 않고**, 어긋나면 바뀔 파일 목록과 함께 exit 1 |
| `--apply` | 마이그레이션을 dev DB 에 실행. `ATLAS_ENV=dev` 필수 |
| `--allow-drops` | `--apply` 와 함께: `MODIFY` / `DROP` 까지 실행. 데이터 손실 경로 |
| `--schema=<path>` | 입력 경로 오버라이드. **테스트 전용** |
| `--cpp-output-dir=<path>` | C++ 출력 경로 오버라이드. **테스트 전용** |
| `--sql-output=<path>` | SQL 출력 경로 오버라이드. **테스트 전용** |
| `--migrations-dir=<path>` | 마이그레이션 출력 경로 오버라이드. **테스트 전용** |
| `--env-file=<path>` | `.env` 경로 오버라이드. **테스트 전용** |

## 규칙

- **생성 출력을 직접 편집하지 않는다.** 고칠 것이 있으면 `server/db/schema.json` 이나 이 생성기를
  고치고 다시 돌린다. 수동 편집은 다음 `gen:db` 에서 사라지고 `gen:db:check` 가 CI 에서 잡는다.
- **타입 매핑을 여기에 복제하지 않는다.** 정규화 → C++/MySQL 은 `tools/types.json` 이 SoT 다.
  `string(N)` · `datetime` · `json` 세 개만 표 밖의 특수 처리이며 emitter 한 곳에서 다룬다.
- **`int` / `long` 을 방출하지 않는다** (`cpp-style.md §4.1`). Windows LLP64 / Linux LP64.
- **`template.ini` 에 접속·환경 키를 넣지 않는다 — 지금 0개이고 계속 0개다.** 이 생성기가 DB 에
  접속하게 된 뒤에도(`design §10.7`) 그 정보는 `.env` 에서 lazy 로 읽는다. `template.ini` 는
  커밋되는 파일이므로 키를 하나 만드는 순간 그 값이 언젠가 레포에 들어온다.
- **데모 최소 집합(`design §3.3`)을 넘겨 스키마를 늘리지 않는다.** 현재 두 장이다 —
  `characters`(복합 PK · 그리드 좌표 · 성장 축 1개)와 `character_items`(인벤토리/장비, 슬롯 3종).
  테이블을 추가하려면 서버 코어의 어떤 미검증 경로를 행사하기 위해서인지 먼저 답해야 한다
  (`character_items` 의 답은 다중 테이블 · 다중 행 트랜잭션 원자성이며 `design §3.3` 에 있다).
- 생성 코드는 `.clang-tidy` 검사 대상이 아니다(`pkt_generator/AGENTS.md` 의 3중 배제와 동일 —
  CI 의 TU 목록이 `server/atlas`·`server/tests` 뿐이고, `HeaderFilterRegex` 가 `/atlas/` 만,
  `ExcludeHeaderFilterRegex` 가 `/generated/` 를 한 번 더 배제한다).
  같은 이유로 손으로 쓴 `tests/db_schema_test.cpp` 도 tidy 를 받지 않는다 — 컴파일러 경고
  (`/W4`)는 그대로 받으므로 경고 없이 빌드돼야 한다.
