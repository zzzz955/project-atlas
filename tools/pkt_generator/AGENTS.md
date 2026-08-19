# pkt_generator — 계약(C#) → C++ 패킷 DTO

계약 한 벌에서 여러 타깃을 뽑는 것이 이 프레임워크의 seam 이다
(`docs/design/architecture-design.md §14`). 이 디렉터리는 그 중 **C++ 타깃**을 소유한다.

> 루트 `tools/AGENTS.md` 는 파이프라인 전체를 설명한다 — 이 문서는 pkt 타깃의 세부다.

## 입력 → 출력

실행은 레포 **루트**에서: `npm run gen:pkt` / 드리프트 검사 `npm run gen:pkt:check`.
경로 · 네임스페이스는 전부 `template.ini [packet-gen]` 이 소유한다 — 하드코딩된 게임/엔진 경로는
없다.

| ini 키 | 값 | 의미 |
|---|---|---|
| `contracts_dir` | `shared/contracts` | 입력. `**/*.cs` 재귀 수집(`bin`/`obj`/`auth` 및 `_` 로 시작하는 이름은 제외) |
| `cpp_output_dir` | `server/generated/pkt` | 출력 |
| `cpp_namespace` | `atlas::generated` | 생성 네임스페이스 |

### 계약 소스가 왜 C# 인가

**재논의 금지(2026-08-06 사용자 확정).** ① 자매 프로젝트의 C# 파서를 그대로 재사용한다
② Phase 2 "기존 게임 1종 이식"이 템플릿화의 진짜 검증인데(`design §14`) 계약 포맷이 갈리면 그
이식이 반쯤 재작성이 된다. C++ 레포에 `.cs` 가 있는 것은 **의도된 것**이다 — 계약 한 벌 →
C#/GDScript/C++ 다중 타깃.

### 인식하는 C# 문법

| 계약 문법 | C++ 산출 |
|---|---|
| `public [sealed\|partial] class X { ... }` | `struct X` + `Write` / `Read` + `operator==` |
| `public T Prop { get; set\|init; }` | 멤버 `prop_` (`cpp-style.md §3` 후행 밑줄) |
| `public enum E : byte { A = 0, B }` | `enum class E : UInt8 { A = 0, B = 1, };` (암묵 값을 **명시로 확정**) |
| `List<T>` · `IList<T>` · `IReadOnlyList<T>` · `IEnumerable<T>` · `T[]` | `std::vector<T>` (길이 프리픽스 + 원소 반복) |
| `string` | `std::string` (UInt16 바이트 프리픽스 + UTF-8) |
| `bool` | `bool` (1바이트) |
| `T?` | `T` (nullable 표기는 무시 — 와이어에 존재 비트를 두지 않는다) |

### 출력 파일

| 파일 | 내용 |
|---|---|
| `pkt_codec.h` | LE `WriteLe`/`ReadLe` 오버로드(8개 폭) · `WriteBool`/`ReadBool` · `WriteLength` · `WriteUtf8`/`ReadUtf8`. 헤더 온리 |
| `pkt_codec.cpp` | 헤더를 자기 완결적으로 만드는 TU 하나. unity-OFF 빌드에서 include 누락을 잡는 게이트다 |
| `contract_enums.h` | 계약의 모든 `enum class` (enum 이 하나라도 있을 때만) |
| `<snake_case>.h` / `.cpp` | DTO 한 클래스당 한 쌍 |
| `pkt_sources.cmake` | `set(ATLAS_GENERATED_PKT_SOURCES ...)`. 소스 목록도 생성물이다 — 계약을 추가해도 CMake 를 손대지 않고, 낡은 목록이 DTO 를 조용히 빠뜨릴 수 없다 |

`server/generated/pkt/CMakeLists.txt` 와 `tests/` 만 손으로 쓴 것이고, 나머지는 전부 생성물이다.

## 와이어 포맷

SoT 는 `design §8.5` 의 "DTO 와이어 포맷 확정" 표다. 폭은 이 파일 한 곳
(`LENGTH_PREFIX_CPP`)에서 나오고, 같은 내용이 생성 코드 상단 주석에 박힌다 — **표 · 상수 · 주석
셋이 함께 움직여야 한다.**

요약: 리틀엔디언 고정 · 길이 프리픽스 `UInt16` LE(문자열=바이트 수, 배열=원소 수, 상한 65535,
초과 시 클램프) · 문자열 UTF-8(종료 문자·BOM 없음) · `bool` 1바이트 · enum 은 계약 선언 폭.

## 이 생성기가 만들지 않는 것

**프레임 헤더 · opcode 테이블 · 시퀀스 넘버 · 체크섬 · HMAC.** `design §8.1` 프레임 필드 폭과
`§16` HMAC/AEAD 가 아직 미결이다. 지금 헤더를 지어내면 계약이 확정될 때 생성기와 생성물을 둘 다
재작업하게 된다. 여기서 나오는 것은 **DTO 뿐**이고, 프레임은 코어의 몫이다.

## 생성기가 거부하는 것 (조용히 통과시키지 않는다)

| 거부 대상 | 이유 |
|---|---|
| `float` / `double` 필드 | `design §8.4` — 부동소수점은 와이어에 싣지 않는다. 양자화된 정수로 고쳐라. 조용히 통과시키면 대역폭이 터지는 부하 테스트에 가서야 드러난다 |
| 매핑 없는 C# 타입 (`DateTime` · `decimal` · 중첩 계약 클래스) | 타입 SoT 는 `tools/types.json` 이다. 4열을 모두 채워 추가하거나 필드를 편다 |
| 중첩 컬렉션 (`List<List<T>>`) | SoA 로 펴라 — `§8.4` 델타 압축이 원하는 배치와 같다 |
| 고정폭 정수가 아닌 enum 기반 타입 | 값이 와이어에 실리므로 폭이 고정돼야 한다(`cpp-style.md §4.4`) |
| 계약 `.cs` 가 0개 | 빈 출력 디렉터리는 모든 DTO 를 빌드에서 조용히 떨어뜨린다 |

거부는 **비제로 exit + `[pkt] ERROR:` 로 시작하는 다중 행 메시지**다. 검증을 전부 끝낸 뒤에
파일을 쓴다 — 뒤쪽 계약이 거부당했는데 앞쪽 DTO 만 새로 쓰인 반쪽 출력이 남으면 안 된다.

## 테스트

| 위치 | 무엇 |
|---|---|
| `server/generated/pkt/tests/pkt_codec_test.cpp` | 라운드트립 · 잘린 입력(모든 접두사가 `false`, throw 없음) · LE 바이트 순서 · UTF-8 프리픽스가 바이트를 센다 · enum 폭 |
| `server/generated/pkt/tests/CMakeLists.txt` | 위 + `pkt.generator_rejects_fp_{exit,message}` — `testdata/fp_reject/` 를 `--contracts-dir` 로 물려 **생성기 자체**를 테스트한다 |

`testdata/` 는 `shared/contracts/` 밖에 있으므로 평소 `npm run gen:pkt` 는 이 픽스처를 보지 않는다.

`ctest --preset windows-ci -R pkt --output-on-failure`

## CLI

| 플래그 | 용도 |
|---|---|
| `--check` | 드리프트 검사만. 파일을 쓰지 않고, 어긋나면 바뀔 파일 목록과 함께 exit 1 |
| `--contracts-dir=<path>` | 입력 경로 오버라이드. **테스트 전용** |
| `--cpp-output-dir=<path>` | 출력 경로 오버라이드. **테스트 전용** |

## 규칙

- **생성 출력을 직접 편집하지 않는다.** 고칠 것이 있으면 계약(`shared/contracts/**/*.cs`)이나
  이 생성기를 고치고 다시 돌린다. 수동 편집은 다음 `gen:pkt` 에서 사라지고 `gen:pkt:check` 가 CI 에서 잡는다.
- **타입 매핑을 여기에 복제하지 않는다.** C# → 정규화 → C++ 는 `tools/types.json` 이 SoT다.
- **`int` / `long` 을 방출하지 않는다** (`cpp-style.md §4.1`). Windows LLP64 / Linux LP64.
- 생성 코드는 `.clang-tidy` 검사 대상이 아니다. **3중으로 배제된다**(2026-08-06 확인):
  ① `.github/workflows/ci.yml` 의 clang-tidy 단계가 `find server/atlas server/tests -name '*.cpp'`
  로 TU 목록을 만든다 — `server/generated/**` 는 애초에 들어가지 않는다
  ② `HeaderFilterRegex: '[/\\]atlas[/\\]'` 는 경로에 `/atlas/` 구획이 있는 헤더만 진단한다
  ③ `ExcludeHeaderFilterRegex: '[/\\]generated[/\\]'` 가 한 번 더 배제한다(`cpp-style.md §7.1`,
  clang-tidy ≥ 19 필요). wp1 산출물을 고칠 필요가 없었다.
- 부수 효과: `server/generated/pkt/tests/pkt_codec_test.cpp` 는 **손으로 쓴 파일인데도**
  `generated/` 아래에 있다는 이유로 위 3중 배제에 함께 걸려 clang-tidy 를 받지 않는다.
  의도한 트레이드다(이 노드가 디렉터리 하나를 통째로 소유해 wp4/wp5 와 파일이 겹치지 않는 쪽을
  택했다). 그래도 컴파일러 경고(`/W4`)는 그대로 받으므로 이 파일과 방출 코드 모두 경고 없이
  빌드돼야 한다.
