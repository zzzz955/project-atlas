# project-atlas — C++ 코딩 컨벤션 SoT

> 작성 2026-08-06 · 이 문서가 컨벤션의 SoT다.
> 🔴 **문서만으로는 지켜지지 않는다.** 실효는 §7의 기계 강제가 만든다. 이 문서는 그 설정의 근거를 적는다.
> 이 문서와 `.clang-tidy` / `.clang-format`은 **프레임워크 자산**이다 — 게임이 프레임워크를 가져가면 함께 따라간다.

---

## 1. 표준

C++20. `target_compile_features(atlas_core PUBLIC cxx_std_20)`로 전 타깃에 상속시킨다.

---

## 2. 네임스페이스

### 2.1 규칙

- 최상위 **`atlas`** 하나 — **필수**
- 하위 모듈 분할(`atlas::net` 등) — **실제 충돌이 생길 때만.** 🔴 미리 나누지 않는다
- 게임 코드는 자기 네임스페이스(예: `mygame`)
- 🔴 **헤더에서 `using namespace` 전면 금지**

### 2.2 왜 필요한가

프레임워크 코드와 게임 코드가 **같은 빌드에 들어간다.** `Session` `Actor` `Logger` `Buffer` `Handler` — 전부 게임 쪽에서도 나올 이름이고, Boost/OpenSSL/MySQL 헤더와도 섞인다. 네임스페이스를 쓰지 않으면 결국 `AtlasSession` 처럼 이름에 접두사를 박게 되는데, 그것은 네임스페이스를 손으로 하는 것이고 더 길며 도구 지원도 없다.

🔴 **unity build가 이를 악화시킨다.** 파일 8개가 한 TU로 합쳐지면 파일 스코프 격리가 사라진다 — 서로 다른 파일의 `static int counter;` 나 익명 네임스페이스 심볼이 **같은 TU에서 재정의 충돌**한다. `using namespace`가 헤더에 있으면 배치 전체로 샌다.

### 2.3 타이핑 비용을 0으로 만드는 설정

스트레스 원인 3가지에 각각 해법이 있다.

| 원인 | 해법 |
|---|---|
| 타이핑 | 하위 분할을 안 하므로 `namespace atlas {` 한 줄. + 에디터 파일 템플릿/스니펫으로 자동 삽입 |
| **들여쓰기 한 단계 소모** | `.clang-format` → `NamespaceIndentation: None` (네임스페이스 안 코드가 들여쓰기 0에서 시작) |
| 닫는 괄호 짝 놓침 | `.clang-format` → `FixNamespaceComments: true` (`} // namespace atlas` 자동 삽입) |

결과:
```cpp
#pragma once
#include "atlas/core/types.h"

namespace atlas {

class SessionManager {
    ...
};

}  // namespace atlas
```
들여쓰기 손해 0, 수작업 2줄.

🔴 **매크로(`ATLAS_NS_BEGIN`/`END`)로 감싸지 않는다** — 매크로 최소주의(§5)와 충돌하고, IDE 인덴트/폴딩/자동완성이 깨지며, 얻는 것은 2줄뿐이다.

---

## 3. 네이밍

| 대상 | 규칙 | 예 |
|---|---|---|
| 파일 | `snake_case` | `session_manager.h` / `.cpp` |
| 네임스페이스 | `snake_case` | `atlas` |
| 타입 · 클래스 · struct · `enum class` · concept · 타입 별칭 | `PascalCase` | `SessionManager` `ActorId` `Int32` |
| 함수 · 메서드 | `PascalCase` | `HandlePacket()` |
| 지역 변수 · 파라미터 | `snake_case` | `packet_size` |
| 멤버 변수 | `snake_case_` (후행 밑줄) | `strand_` `session_id_` |
| 정적/전역 상수 · `constexpr` | `kPascalCase` | `kMaxPacketSize` |
| `enum class` 값 | `PascalCase` | `State::Connected` |
| **매크로** | `ATLAS_UPPER_SNAKE` (prefix 필수) | `ATLAS_ASSERT` |
| 템플릿 파라미터 | `PascalCase` | `template<class Handler>` |

**후행 밑줄 멤버를 고른 이유** — ① clang-tidy가 기계적으로 검사 가능하다 ② 생성자에서 파라미터와 멤버가 이름 충돌 없이 읽힌다(`session_id_{session_id}`).

---

## 4. 타입 규칙

네트워크 서버에서는 네이밍보다 이쪽이 중요하다.

### 4.1 고정폭 정수 필수 — `int`/`long` 금지

프로토콜 · 영속 · ID 계층에서 `int`/`long`을 쓰지 않는다.

🔴 **이유가 치명적이다: Windows는 LLP64, Linux는 LP64라 `long`이 4바이트 vs 8바이트다.** 이 프로젝트는 dev(Windows)와 prod(Linux Docker)를 반드시 오가므로, 구조체 크기와 직렬화 결과가 환경에 따라 갈린다. 예외 없다.

### 4.2 타입 별칭 (A안 — PascalCase)

`Int32` / `UInt64` 형태. **"타입 = PascalCase"** 규칙과 일관되므로 별도 예외 조항이 생기지 않는다.

🔴 **파일을 3개로 나눈다.** 하나로 합치면 빌드 시간 이점이 사라진다.

#### `atlas/core/types.h` — `<cstdint>` `<cstddef>` 만 의존
```cpp
#pragma once
#include <cstdint>
#include <cstddef>

namespace atlas {

using Int8  = std::int8_t;    using UInt8  = std::uint8_t;
using Int16 = std::int16_t;   using UInt16 = std::uint16_t;
using Int32 = std::int32_t;   using UInt32 = std::uint32_t;
using Int64 = std::int64_t;   using UInt64 = std::uint64_t;

using Float32 = float;
using Float64 = double;
using Byte    = std::byte;

}  // namespace atlas
```
🔴 **이 헤더에 asio/chrono를 넣지 않는다.** 이 파일이 값싼 이유는 `<cstdint>`/`<cstddef>`만 의존하기 때문이다. `<boost/asio.hpp>`가 들어오면 모든 파일이 asio 전체를 파싱하게 되어 PCH·unity build로 아낀 시간을 통째로 반납한다.

🔴 **`std::size_t`의 별칭은 만들지 않는다** — 이미 짧고, 표준 컨테이너 API와 직접 맞물리는 자리라 별칭이 오히려 혼선을 만든다.

#### `atlas/core/time.h` — `<chrono>` 만 의존
```cpp
namespace atlas {

using Clock     = std::chrono::steady_clock;   // 단조 — 틱 · 타임아웃 · 지연 측정
using TimePoint = Clock::time_point;
using Duration  = Clock::duration;

using SysClock  = std::chrono::system_clock;   // 벽시계 — DB 저장 · 로그 타임스탬프
using SysTime   = SysClock::time_point;

using Nanos   = std::chrono::nanoseconds;
using Micros  = std::chrono::microseconds;
using Millis  = std::chrono::milliseconds;
using Seconds = std::chrono::seconds;

}  // namespace atlas
```
🔴 **두 시계를 섞지 않는다.** 타임아웃 · 틱 간격 · 지연 측정은 `Clock`(steady), 저장 · 표시는 `SysClock`. `system_clock`은 NTP 보정으로 시간이 뒤로 갈 수 있어 타임아웃 계산에 쓰면 무한 대기가 발생한다.

⚠️ 리터럴(`100ms`)은 `using namespace std::chrono_literals`가 필요한데 §2.1의 헤더 금지 규칙이 우선한다 → **헤더에서는 `Millis{100}`, `.cpp`에서만 리터럴 허용.**

#### `atlas/net/net_types.h` — asio 의존, 네트워크 계층 전용
```cpp
namespace atlas {

namespace asio = boost::asio;   // 네임스페이스 별칭 (using-directive 아님 — 심볼 누출 없음)

using ErrorCode   = boost::system::error_code;
using IoContext   = asio::io_context;
using Strand      = asio::strand<asio::io_context::executor_type>;
using Tcp         = asio::ip::tcp;
using Socket      = Tcp::socket;
using Acceptor    = Tcp::acceptor;
using Endpoint    = Tcp::endpoint;
using SteadyTimer = asio::steady_timer;

}  // namespace atlas
```
`namespace asio = boost::asio;` 는 **네임스페이스 별칭**이므로 `using namespace`와 달리 심볼을 뿌리지 않는다 — 헤더에서 안전하다.

#### 별칭 원칙 2가지 (없으면 별칭이 무한 증식한다)
1. **자주 쓰이고 이름이 긴 것만.** 1~2회 쓰는 것은 원본 그대로.
2. **재명명만 하고 의미를 바꾸지 않는다.** 별칭 뒤에 정책을 숨기면 읽는 사람이 원본을 찾지 못한다.

### 4.3 강타입 ID — 가장 값싸고 효과 큰 규칙

```cpp
enum class ActorId     : UInt64 {};
enum class SessionId   : UInt64 {};
enum class CharacterId : UInt64 {};
```

전부 `UInt64` 별칭으로 두면 `Foo(session_id, actor_id)` 에 인자 순서를 바꿔 넣어도 **컴파일이 통과하고 런타임에 조용히 틀린다.** `enum class`는 암묵 변환이 없어 이를 컴파일 에러로 만들며 **런타임 비용은 0이다.**

### 4.4 그 외

| 항목 | 규칙 | 이유 |
|---|---|---|
| **패킷 구조체** | 🔴 `#pragma pack` 금지. `pkt_generator`가 **필드별 write/read 생성** | 패딩·정렬이 컴파일러/플랫폼마다 다르다 |
| **엔디언** | **리틀엔디언 고정** | x86/ARM 모두 LE → 변환 비용 0. 🔴 "정했다"를 명시하지 않으면 반드시 혼선이 난다 |
| **부동소수점** | 🔴 프로토콜에 싣지 않는다. 서버 내부는 `Float32`, 전송은 **양자화된 정수** | 표현 차이 + 대역폭 |
| **소유권** | `unique_ptr`=단독 / `shared_ptr`=공유 / `T*`=**비소유 관찰자** / `T&`=null 불가 비소유 / `optional`=값 없음 가능 | 소유권을 타입으로 표현한다 |
| **asio 세션** | `enable_shared_from_this` 필수 | 핸들러 실행 중 세션 소멸 방지 |
| **버퍼** | `std::span<const Byte>`. 🔴 `char*` 금지 | `char`는 부호가 구현 정의이고 길이가 따라다니지 않는다 |
| **문자열** | 파라미터 `std::string_view`, 소유 `std::string` | 🔴 `string_view`를 멤버로 보관 금지 (수명 함정) |
| **enum** | `enum class` + 기반 타입 명시 | 프로토콜에 실리므로 크기가 고정돼야 한다 |
| **`auto`** | 반복자 · 긴 템플릿 타입에만. 🔴 **수치 타입에 금지** | `auto x = 0;` 은 `int`가 되어 §4.1을 우회한다 |
| **부호 없는 뺄셈** | 길이 검증을 뺄셈 **전에** | `size_t a - size_t b`가 음수면 거대한 양수로 언더플로 |

---

## 5. 매크로 정책 — 최소주의

**허용 화이트리스트만:**
```cpp
ATLAS_LOG_TRACE / DEBUG / INFO / WARN / ERROR / FATAL
ATLAS_THROW(Type, fmt, ...)   // source_location + ctx 첨부하여 throw
ATLAS_CHECK(cond, fmt, ...)   // 실패 시 ATLAS_THROW. 릴리즈에도 살아있음
ATLAS_ASSERT(cond)            // 디버그 전용, 릴리즈에서 소거
```
+ 플랫폼 분기 · 빌드 설정 플래그

**규칙**
- 🔴 위 외의 매크로 금지. 특히 상수/인라인 함수 대체용 매크로 금지 → `constexpr` / `inline`
- prefix `ATLAS_` 필수
- `Windows.h`는 `NOMINMAX` + `WIN32_LEAN_AND_MEAN` 강제 (unity build 오염 방지와 직결. 코어 타깃이 `PUBLIC`으로 전파)

**왜 로그만 매크로가 정당한가**
```cpp
ATLAS_LOG_DEBUG("actor={} dump={}", id, ExpensiveDump(a));
```
로그 레벨이 꺼져 있으면 **`ExpensiveDump()`가 아예 평가되면 안 된다.** 함수로는 불가능하다(인자가 먼저 평가된다). 릴리즈에서 TRACE/DEBUG를 `#if`로 통째 제거하는 것도 매크로만 가능하다. C++20 `std::source_location`이 파일/라인/함수 캡처는 매크로 없이 해결하므로, **매크로의 근거는 지연 평가 하나뿐이며 그것으로 충분하다.**

🔴 **순수 매크로로 `try/catch`를 감싸지 않는다** — 중괄호 짝 · `return` 처리 · 중첩에서 지저분해지고 디버거가 따라가지 못한다. 핸들러 경계는 템플릿 래퍼 `Guarded`가 담당한다(설계 문서 §11.2).

---

## 6. 템플릿 정책 — 빌드 시간이 정책의 근거

- 템플릿은 **코어 유틸에만.** 게임 로직은 구체 타입으로 쓴다 (헤더 비대화 = 빌드 시간 폭발)
- 제약은 **concepts로 명시.** 🔴 공개 API에 SFINAE 금지 (에러 메시지 지옥)
- 반복 인스턴스화가 무거운 것은 `extern template` + 명시적 인스턴스화로 중복 억제
- 타입 소거가 가능한 곳은 템플릿보다 `std::function`/인터페이스 우선 — **컴파일 시간 vs 런타임 비용 트레이드를 암묵이 아니라 명시적으로** 선택한다

---

## 7. 강제 구조 — 3층

🔴 이 절이 컨벤션의 실효다. 문서(§1~6)만 있으면 지켜지지 않는다.

### 7.1 층 1 — 파일 기반 검사

**`.clang-tidy`** — 네이밍을 실제로 강제하는 유일한 수단
```yaml
Checks: >
  readability-identifier-naming,
  cppcoreguidelines-macro-usage,
  bugprone-*, performance-*, modernize-*

HeaderFilterRegex: '[/\\]atlas[/\\]'
ExcludeHeaderFilterRegex: '[/\\]generated[/\\]'

CheckOptions:
  - { key: readability-identifier-naming.ClassCase,             value: CamelCase }
  - { key: readability-identifier-naming.FunctionCase,          value: CamelCase }
  - { key: readability-identifier-naming.TypeAliasCase,         value: CamelCase }
  - { key: readability-identifier-naming.VariableCase,          value: lower_case }
  - { key: readability-identifier-naming.PrivateMemberSuffix,   value: _ }
  - { key: readability-identifier-naming.NamespaceCase,         value: lower_case }
  - { key: readability-identifier-naming.MacroDefinitionCase,   value: UPPER_CASE }
  - { key: readability-identifier-naming.MacroDefinitionPrefix, value: ATLAS_ }
  - { key: readability-identifier-naming.GlobalConstantPrefix,  value: k }
```
🔴 **생성 코드는 반드시 제외한다** — `HeaderFilterRegex`로 `atlas/`만 포함하고, `ExcludeHeaderFilterRegex`로 `generated/`를 배제한다. 🔴 배제를 `HeaderFilterRegex` 하나로 처리할 수 없다 — clang-tidy가 쓰는 `llvm::Regex`는 negative lookahead를 지원하지 않는다.

🔴 **`ExcludeHeaderFilterRegex`는 clang-tidy ≥ 19를 요구한다.** 그 미만에서는 키가 조용히 무시되어 생성 코드가 그대로 검사에 걸린다 → **CI 이미지의 clang-tidy를 19 이상으로 고정할 것.**

**`.clang-format`** — 포맷을 논쟁 대상에서 완전히 제거
```yaml
NamespaceIndentation: None
FixNamespaceComments: true
```

**`.editorconfig`** — 개행 · 인코딩 · 들여쓰기

### 7.2 층 2 — CMake 타깃 상속

🔴 **거대 공통 헤더를 배포하지 않는다** (빌드 시간 폭탄 + 결합도). 대신 코어 타깃이 필수 설정을 `INTERFACE`/`PUBLIC`으로 전파한다:

```cmake
add_library(atlas_core ...)
target_compile_features(atlas_core PUBLIC cxx_std_20)
target_compile_definitions(atlas_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(atlas_core PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)
```

🔴 **`/utf-8`은 장식이 아니다.** `.editorconfig`가 소스를 UTF-8로 고정하지만 MSVC는 그 사실을 모르고 머신의 ANSI 코드페이지(한국어 개발기라면 949)로 디코드해 C4819를 낸다. 주석 끝의 UTF-8 선행 바이트가 개행을 삼키면 **다음 줄 코드가 주석으로 먹히는** 사고가 가능하므로, 경고 억제가 아니라 정확성 문제다. `PUBLIC`이라 코어를 링크하는 모든 타깃이 함께 받는다.

`target_link_libraries(atlas_world PRIVATE atlas_core)` 한 줄이면 **모든 서버가 같은 표준 · 경고 · 정의를 자동 상속**한다. 새 서버를 추가해도 설정을 잊을 수가 없다.

### 7.3 층 3 — CI 게이트 + pre-commit

```
format-check → clang-tidy → build(unity ON) → build(unity OFF) → test
```

- 🔴 **게이트가 없으면 clang-tidy도 돌지 않고 문서는 장식이 된다**
- 🔴 **non-unity 빌드가 "누락 include / ODR 충돌" 게이트를 겸한다** (설계 문서 §15.1 대가 4번). 이 게이트가 없으면 반드시 터진다
- **pre-commit hook**: 변경 파일만 format + tidy. CI보다 먼저 잡아 왕복 비용을 줄인다

#### 🔴 `clang-tidy`는 CI(Linux/clang) 전용이다 — 로컬 Windows 게이트에서는 건너뛴다

실측 2026-08-06 (wp3 STAGE 2). MSVC(`cl.exe`)로 구성한 `compile_commands.json`에 대해
clang-tidy 19.1.5를 돌리면 CMake의 PCH 산출물에서 멈춘다:

```
error: file '.../build/windows-ci/atlas/core/CMakeFiles/atlas_core.dir/cmake_pch.cxx.pch'
       is not a valid precompiled PCH file: file doesn't start with AST file magic
error: input is not a PCH file: '.../cmake_pch.cxx.pch'
```

`target_precompile_headers`가 MSVC에서 만드는 `.pch`는 `/Yc` 바이너리이고 clang은 자기 AST 포맷만
읽는다. **PCH를 뺀 나머지 MSVC 인자는 clang-tidy가 정상 파싱한다** — `/Y-`로 PCH만 억제하면 같은
호출이 `--warnings-as-errors=*`에서 exit 0이므로, 비호환은 PCH 하나로 한정된다.

- 🔴 **그래서 `--extra-arg=/Y-`를 끼워 넣지 않는다.** 그렇게 하면 **컴파일러가 실제로 빌드하지 않는
  translation unit**을 검사하게 된다 — PCH가 주입하던 것과 다른 전처리 상태를 보므로, 통과해도
  실제 빌드를 보증하지 못하고 반대로 없는 문제를 신고한다. 게이트가 거짓 신호를 주는 쪽이
  게이트가 없는 것보다 나쁘다.
- 🔴 **PCH를 끄는 것도 답이 아니다.** PCH는 §15.1에서 빌드 시간을 위해 명시적으로 선택한 것이고,
  린터 하나 때문에 되돌릴 대상이 아니다.
- ✅ **결정: 강제 지점을 `linux-ci`로 옮긴다.** `.github/workflows/ci.yml`의 clang-tidy 단계는
  clang이 만든 compile db와 clang이 읽을 수 있는 PCH를 쓰므로 그대로 성립한다.
  `server/scripts/ci-gate.ps1`은 이 단계를 **사유를 출력하고** 건너뛴다 — 조용한 스킵은 게이트가
  죽은 것을 감춘다.
- **대가(감수한다)**: git remote가 붙기 전까지 §7.1의 네이밍 · `bugprone-*` · `modernize-*` 강제가
  로컬에서 돌지 않는다. 로컬에 남는 기계 강제는 `.clang-format`(층 1) + §7.2 경고 옵션 + unity
  ON/OFF 두 빌드다.
- **해소 조건**: 로컬에 clang-cl 도입. clang-cl 프리셋은 clang이 읽는 PCH를 만들므로 그 시점에
  이 단계를 `ci-gate.ps1`으로 되돌린다.

---

## 8. 프레임워크 자산 목록

새 게임이 이 프레임워크를 가져갈 때 **함께 따라가는 파일**:

```
.clang-format
.clang-tidy
.editorconfig
server/CMakePresets.json
CI 워크플로
docs/conventions/cpp-style.md          ← 이 문서
server/atlas/core/types.h              (A안 별칭)
server/atlas/core/time.h               (chrono 별칭)
server/atlas/core/ids.h                (강타입 ID 4종 + IdValue)
server/atlas/core/ctx.h  /  .cpp       (ctx 원장 + CtxScope RAII 설치/복원)
server/atlas/core/log.h  /  .cpp       (매크로 6종, spdlog 래핑 — spdlog는 .cpp에 은닉)
server/atlas/core/error.h  /  .cpp     (ATLAS_THROW / CHECK / ASSERT + Guarded)
server/atlas/config/ini_document.h    /  .cpp   (손으로 쓴 최소 ini 파서 — 의존성 0)
server/atlas/config/server_config.h   /  .cpp   (ServerRole · [stack] 축 · 로그 설정)
server/atlas/config/secret_config.h   /  .cpp   (.env 시크릿 — 🔴 값은 로그·예외에 싣지 않는다)
server/server.ini                      (커밋되는 런타임 설정. 설계 문서 §5.4)
server/atlas/net/net_types.h           (asio 별칭)
server/atlas/net/io_runner.h  /  .cpp  (io_context + 워커 풀 + graceful stop)
server/atlas/net/session.h    /  .cpp  (strand 세션 — 바이트 스트림까지, 프레이밍 없음)
server/atlas/net/acceptor.h   /  .cpp  (accept 루프 + 세션 생성)
```

🔴 **net 3종이 목록에 있는 이유는 "게임이 바뀌어도 안 바뀌기 때문"이다.** 이 파일들의 경계는
**바이트 스트림까지**이며 프레임 · 페이로드 식별자 · 무결성 필드를 일절 모른다(설계 문서 §8은 별도
계층). 게임이 프로토콜을 바꿔도 이 3종은 그대로 따라간다 — 그 경계를 무너뜨리는 순간 자산 목록에서
빠져야 하는 파일이 된다.

경로에 `server/` 접두가 붙는 이유는 CMake 루트이자 include 루트가 `server/`이기 때문이다 —
include 지점은 `#include "atlas/core/types.h"`로 쓰지만 파일은 `server/atlas/...`에 있다.

이 목록이 따라가므로 **"게임마다 스타일이 갈리는" 상황이 구조적으로 불가능해진다.**
