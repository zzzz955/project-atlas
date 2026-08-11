# info_generator — 기획 CSV → 컴파일-인 C++ 정적 데이터 테이블

CSV 한 벌에서 여러 타깃을 뽑는 것이 이 프레임워크의 seam 이다
(`docs/design/architecture-design.md §14`). 이 디렉터리는 그 중 **서버 C++ 타깃**을 소유한다.

> 루트 `tools/AGENTS.md` 는 파이프라인 전체를 설명한다 — 이 문서는 info 타깃의 세부다.

🔴 **DB 테이블이 아니다.** 행은 바이너리에 박히고 런타임 로드가 없다. 그래서 `db_generator` 와 달리
SQL 도 · 커넥션도 · 마이그레이션도 없다. 이 성질이 소비처를 결정한다 — 환경 때문에 실패할 수 없는
조회이므로 **트랜잭션 앞에 설 수 있다**(`§8.2` 3층 서버 권위, `server/game/equip_service.cpp`).

## 입력 → 출력

실행은 레포 **루트**에서: `npm run gen:info` / 드리프트 검사 `npm run gen:info:check`.
경로 · 네임스페이스 · 타깃은 전부 `template.ini [data-gen]` 이 소유한다 — 🔴 하드코딩 경로는 없다.

| ini 키 | 값 | 의미 |
|---|---|---|
| `[paths].datas_dir` | `shared/datas` | 입력 루트(재귀 스캔, `*.csv`) |
| `server_targets` | `S,CS` | CSV 2행의 타깃 중 서버가 가져갈 것 |
| `cpp_info_output_dir` | `server/generated/info` | C++ 출력 |
| `cpp_namespace` | `atlas::generated` | 생성 네임스페이스 |

### 입력 포맷 — 5행 헤더 (자매 게임 `project-tower` 와 동일)

```
id,name,slot                      ← 1행 필드명   (^[a-z][a-z0-9_]*$, 중복 불가)
CS,C,CS                           ← 2행 타깃     (C 클라전용 / S 서버전용 / CS 공유)
uint32,string,uint8               ← 3행 정규화 타입 (tools/types.json 의 키)
PK,NN,NN                          ← 4행 제약     (PK / UQ / NN / 공백)
1001,Rusty Sword,1                ← 5행~ 데이터
```

🔴 **`C` 전용 열은 서버 emit 에서 빠진다.** 클라 emit 은 이 슬라이스 밖이고, **클라가 C++ 이라는
가정은 필요 없다** — 타깃별 emit 언어는 생성기의 출력 타깃이지 CSV 계약이 아니다(자매 게임은 같은
CSV 로 GDScript 를 뽑는다). 그래서 `pkt_consts` 류 공유 구조를 만들지 않는다.

🔴 **타입은 `tools/types.json` 의 정규화 타입 키만 허용한다.** `int` / `long` / `varchar` 표기는
애초에 존재하지 않으므로 자동으로 거부된다 — `cpp-style.md §4.1` 금지가 데이터 층에서도 **기계로**
지켜지는 지점이다.

### 출력 파일

🔴 **CSV 1장 = C++ TU 1개.** 산출물 구조는 `server/generated/db/` 를 그대로 본떴다.

| 파일 | 내용 | db 쪽 대응 |
|---|---|---|
| `info_meta.h` | `enum class InfoColumnType : UInt8` · `struct InfoColumnMeta` | `db_meta.h` |
| `info_meta.cpp` | 헤더를 자기 완결적으로 만드는 TU 하나 | `db_meta.cpp` |
| `<name>_info.h` | 행 구조체 · 폭 `static_assert` · 컬럼 인덱스 상수 · `std::array<InfoColumnMeta,N>` · PK 조회 API 선언 | `<table>_row.h` |
| `<name>_info.cpp` | 데이터 실체(`constexpr std::array`) + PK 이분 탐색. 🔴 unity-OFF 에서 include 누락을 잡는 TU | `<table>_row.cpp` |
| `info_all.h` | 🔴 **집합 헤더** — 전체 `<name>_info.h` 를 include. "전역 사용 가능"의 실체이며 **이것도 생성물**이다 | (db 에는 없음) |
| `info_sources.cmake` | `set(ATLAS_GENERATED_INFO_SOURCES ...)`. 🔴 소스 목록도 생성물 — CSV 를 추가해도 CMake 를 손대지 않는다 | `db_sources.cmake` |

`server/generated/info/CMakeLists.txt` 만 손으로 쓴 것이고, 나머지는 전부 생성물이다.

### 🔴 `InfoColumnType` / `InfoColumnMeta` 이름이 db 와 다른 이유

두 생성기가 **같은 네임스페이스**(`atlas::generated`)로 방출하므로 `ColumnType` · `ColumnMeta` 를
그대로 쓰면 행 헤더와 info 헤더를 함께 include 하는 TU 에서 재정의가 된다 — 장착 경로가 실제로 그
TU 다. info 가 db 출력을 include 하게 만드는 쪽은 필드 4개짜리 구조체 하나 때문에 독립된 두 생성기를
묶는 것이라 택하지 않았다.

### 🔴 텍스트 열이 `std::string_view` 인 이유

`cpp-style.md §4.4` 는 `string_view` 를 **멤버로 보관하는 것**을 금지한다. 그 근거는 뷰가 소유자보다
오래 사는 수명 함정인데, 여기 소유자는 **바이너리 안의 문자열 리터럴**(정적 저장 기간)이라 넘어설
대상이 없다. `std::string` 으로 소유하면 이 행들이 `constexpr` 밖으로 나가는데, 그 대가로 얻는 것이
없다.

## 🔴 생성기가 거부하는 것 (조용히 통과시키지 않는다)

거부는 **`[info] ERROR:` + `파일:행:열` + 기대값 + 실제값**이며 exit 1 이다. 🔴 예외를 throw 하고
**스택도 같이 찍는다** — 메시지만 찍고 넘어가면 데이터 오류와 생성기 자체의 버그를 구분할 수 없다.
🔴 검증을 전부 끝낸 뒤에 파일을 쓴다 — 뒤쪽 CSV 가 거부당했는데 앞쪽 테이블만 새로 쓰인 반쪽 출력이
남으면 안 된다.

| 거부 대상 | 픽스처 (`testdata/`) |
|---|---|
| 헤더 4행 + 데이터 1행 미만 (제약 행 누락 등) | `missing_constraint_row/` |
| 타깃 표기 오타 (`SC` · `X`) | `bad_target/` |
| `types.json` 에 없는 타입 (`int` · `varchar`) | `unknown_type/` |
| `PK` 열이 정확히 1개가 아님 | `no_primary_key/` |
| PK 값 중복 (`UQ` 열의 값 중복도 같은 검사) | `duplicate_key/` |
| 행별 셀 수 불일치 | `cell_count/` |
| 타입 파싱 실패 (`uint8` 열의 `-1` · `abc`) | `bad_value/` |
| PK 열이 서버 타깃이 아님 (`C`) | — (키 없는 서버 테이블은 조회 API 가 없다) |
| 필드명 규칙 위반 · 중복, CSV 파일명 규칙 위반, `*.csv` 0개 | — |

🔴 **범위는 여기까지다** — 경고 수준 진단 · 자동 수정 제안 · 오류 복구는 전부 제외한다.

🔴 **C 전용 열의 값도 파싱 검사를 받는다.** 서버가 버릴 열이라는 이유로 오타를 통과시키면, 그 오타는
클라 타깃이 붙는 날 한꺼번에 쏟아진다.

## 테스트

| 위치 | 무엇 |
|---|---|
| `server/tests/info_table_test.cpp` | ① PK 조회가 CSV 를 **손으로 옮겨 적은** 기대표와 일치 ② 없는 id 는 nullptr(이웃 행도, 예외도 아님) ③ 행이 PK 오름차순 ④ `C` 열이 서버 구조체에 없음 + 고정폭 타입 |
| `server/tests/CMakeLists.txt` | `info.generator_rejects_<픽스처>_{exit,location}` — 픽스처 7종 × 2. exit 비제로와 `파일:행:열` 을 **따로** 단언한다(`PASS_REGULAR_EXPRESSION` 은 종료 코드를 무시한다) |

`testdata/` 는 `shared/datas/` 밖에 있으므로 평소 `npm run gen:info` 는 이 픽스처를 보지 않고, 출력도
빌드 트리로 빠져 `server/generated/info/` 를 건드리지 않는다.

`ctest --preset windows-ci -R info --output-on-failure`

## CLI

| 플래그 | 용도 |
|---|---|
| `--check` | 드리프트 검사만. 파일을 쓰지 않고, 어긋나면 바뀔 파일 목록과 함께 exit 1 |
| `--datas-dir=<path>` | 입력 경로 오버라이드. **테스트 전용** |
| `--cpp-output-dir=<path>` | 출력 경로 오버라이드. **테스트 전용** |

## 🔴 규칙

- **생성 출력을 직접 편집하지 않는다.** 고칠 것이 있으면 `shared/datas/*.csv` 나 이 생성기를 고치고
  다시 돌린다. 수동 편집은 다음 `gen:info` 에서 사라지고 `gen:info:check` 가 CI 에서 잡는다.
- **타입 매핑을 여기에 복제하지 않는다.** 정규화 → C++ 는 `tools/types.json` 이 SoT 다.
- 🔴 **`int` / `long` 을 방출하지 않는다** (`cpp-style.md §4.1`). Windows LLP64 / Linux LP64.
- 🔴 **데모 최소 집합(`design §3.3`)을 넘겨 CSV 를 늘리지 않는다.** 현재 한 장이며(`item.csv`)
  슬롯 3종·테이블 1개가 상한이다. CSV 를 추가하려면 서버 코어의 **어떤 미검증 경로를 행사하기
  위해서인지** 먼저 답해야 한다(`item.csv` 의 답은 `§8.2` 3층 서버 권위이며 `design §3.3` 에 있다).
- 생성 코드는 `.clang-tidy` · `clang-format` 검사 대상이 아니다(`ci-gate.ps1` 의 `$sourceGlobs` 에
  `generated/` 가 없고, `HeaderFilterRegex` 가 `/atlas/` 만, `ExcludeHeaderFilterRegex` 가
  `/generated/` 를 한 번 더 배제한다). 컴파일러 경고(`/W4`)는 그대로 받으므로 경고 없이 빌드돼야 한다.
