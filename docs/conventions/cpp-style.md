# project-atlas — C++ 코딩 컨벤션 SoT

> 작성 2026-08-06 · 이 문서가 컨벤션의 SoT다.
> **문서만으로는 지켜지지 않는다.** 실효는 §7의 기계 강제가 만든다. 이 문서는 그 설정의 근거를 적는다.
> 이 문서와 `.clang-tidy` / `.clang-format`은 **프레임워크 자산**이다 — 게임이 프레임워크를 가져가면 함께 따라간다.

---

## 1. 표준

C++20. `target_compile_features(atlas_core PUBLIC cxx_std_20)`로 전 타깃에 상속시킨다.

---

## 2. 네임스페이스

### 2.1 규칙

- 최상위 **`atlas`** 하나 — **필수**
- 하위 모듈 분할(`atlas::net` 등) — **실제 충돌이 생길 때만.** 미리 나누지 않는다
- 게임 코드는 자기 네임스페이스(예: `mygame`)
- **헤더에서 `using namespace` 전면 금지**

### 2.2 왜 필요한가

프레임워크 코드와 게임 코드가 **같은 빌드에 들어간다.** `Session` `Actor` `Logger` `Buffer` `Handler` — 전부 게임 쪽에서도 나올 이름이고, Boost/OpenSSL/MySQL 헤더와도 섞인다. 네임스페이스를 쓰지 않으면 결국 `AtlasSession` 처럼 이름에 접두사를 박게 되는데, 그것은 네임스페이스를 손으로 하는 것이고 더 길며 도구 지원도 없다.

**unity build가 이를 악화시킨다.** 파일 8개가 한 TU로 합쳐지면 파일 스코프 격리가 사라진다 — 서로 다른 파일의 `static int counter;` 나 익명 네임스페이스 심볼이 **같은 TU에서 재정의 충돌**한다. `using namespace`가 헤더에 있으면 배치 전체로 샌다.

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

**매크로(`ATLAS_NS_BEGIN`/`END`)로 감싸지 않는다** — 매크로 최소주의(§5)와 충돌하고, IDE 인덴트/폴딩/자동완성이 깨지며, 얻는 것은 2줄뿐이다.

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

**이유가 치명적이다: Windows는 LLP64, Linux는 LP64라 `long`이 4바이트 vs 8바이트다.** 이 프로젝트는 dev(Windows)와 prod(Linux Docker)를 반드시 오가므로, 구조체 크기와 직렬화 결과가 환경에 따라 갈린다. 예외 없다.

### 4.2 타입 별칭 (A안 — PascalCase)

`Int32` / `UInt64` 형태. **"타입 = PascalCase"** 규칙과 일관되므로 별도 예외 조항이 생기지 않는다.

**파일을 3개로 나눈다.** 하나로 합치면 빌드 시간 이점이 사라진다.

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
**이 헤더에 asio/chrono를 넣지 않는다.** 이 파일이 값싼 이유는 `<cstdint>`/`<cstddef>`만 의존하기 때문이다. `<boost/asio.hpp>`가 들어오면 모든 파일이 asio 전체를 파싱하게 되어 PCH·unity build로 아낀 시간을 통째로 반납한다.

**`std::size_t`의 별칭은 만들지 않는다** — 이미 짧고, 표준 컨테이너 API와 직접 맞물리는 자리라 별칭이 오히려 혼선을 만든다.

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
**두 시계를 섞지 않는다.** 타임아웃 · 틱 간격 · 지연 측정은 `Clock`(steady), 저장 · 표시는 `SysClock`. `system_clock`은 NTP 보정으로 시간이 뒤로 갈 수 있어 타임아웃 계산에 쓰면 무한 대기가 발생한다.

리터럴(`100ms`)은 `using namespace std::chrono_literals`가 필요한데 §2.1의 헤더 금지 규칙이 우선한다 → **헤더에서는 `Millis{100}`, `.cpp`에서만 리터럴 허용.**

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
| **패킷 구조체** | `#pragma pack` 금지. `pkt_generator`가 **필드별 write/read 생성** | 패딩·정렬이 컴파일러/플랫폼마다 다르다 |
| **엔디언** | **리틀엔디언 고정** | x86/ARM 모두 LE → 변환 비용 0. "정했다"를 명시하지 않으면 반드시 혼선이 난다 |
| **부동소수점** | 프로토콜에 싣지 않는다. 서버 내부는 `Float32`, 전송은 **양자화된 정수** | 표현 차이 + 대역폭 |
| **소유권** | `unique_ptr`=단독 / `shared_ptr`=공유 / `T*`=**비소유 관찰자** / `T&`=null 불가 비소유 / `optional`=값 없음 가능 | 소유권을 타입으로 표현한다 |
| **asio 세션** | `enable_shared_from_this` 필수 | 핸들러 실행 중 세션 소멸 방지 |
| **버퍼** | `std::span<const Byte>`. `char*` 금지 | `char`는 부호가 구현 정의이고 길이가 따라다니지 않는다 |
| **문자열** | 파라미터 `std::string_view`, 소유 `std::string` | `string_view`를 멤버로 보관 금지 (수명 함정) |
| **enum** | `enum class` + 기반 타입 명시 | 프로토콜에 실리므로 크기가 고정돼야 한다 |
| **`auto`** | 반복자 · 긴 템플릿 타입에만. **수치 타입에 금지** | `auto x = 0;` 은 `int`가 되어 §4.1을 우회한다 |
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
- 위 외의 매크로 금지. 특히 상수/인라인 함수 대체용 매크로 금지 → `constexpr` / `inline`
- prefix `ATLAS_` 필수
- `Windows.h`는 `NOMINMAX` + `WIN32_LEAN_AND_MEAN` 강제 (unity build 오염 방지와 직결. 코어 타깃이 `PUBLIC`으로 전파)

**왜 로그만 매크로가 정당한가**
```cpp
ATLAS_LOG_DEBUG("actor={} dump={}", id, ExpensiveDump(a));
```
로그 레벨이 꺼져 있으면 **`ExpensiveDump()`가 아예 평가되면 안 된다.** 함수로는 불가능하다(인자가 먼저 평가된다). 릴리즈에서 TRACE/DEBUG를 `#if`로 통째 제거하는 것도 매크로만 가능하다. C++20 `std::source_location`이 파일/라인/함수 캡처는 매크로 없이 해결하므로, **매크로의 근거는 지연 평가 하나뿐이며 그것으로 충분하다.**

**순수 매크로로 `try/catch`를 감싸지 않는다** — 중괄호 짝 · `return` 처리 · 중첩에서 지저분해지고 디버거가 따라가지 못한다. 핸들러 경계는 템플릿 래퍼 `Guarded`가 담당한다(설계 문서 §11.2).

---

## 6. 템플릿 정책 — 빌드 시간이 정책의 근거

- 템플릿은 **코어 유틸에만.** 게임 로직은 구체 타입으로 쓴다 (헤더 비대화 = 빌드 시간 폭발)
- 제약은 **concepts로 명시.** 공개 API에 SFINAE 금지 (에러 메시지 지옥)
- 반복 인스턴스화가 무거운 것은 `extern template` + 명시적 인스턴스화로 중복 억제
- 타입 소거가 가능한 곳은 템플릿보다 `std::function`/인터페이스 우선 — **컴파일 시간 vs 런타임 비용 트레이드를 암묵이 아니라 명시적으로** 선택한다

---

## 7. 강제 구조 — 3층

이 절이 컨벤션의 실효다. 문서(§1~6)만 있으면 지켜지지 않는다.

### 7.1 층 1 — 파일 기반 검사

**`.clang-tidy`** — 네이밍을 실제로 강제하는 유일한 수단
```yaml
Checks: >
  readability-identifier-naming,
  cppcoreguidelines-macro-usage,
  bugprone-*, performance-*, modernize-*,
  -modernize-use-trailing-return-type,
  -bugprone-easily-swappable-parameters

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
  - { key: readability-identifier-naming.ConstexprVariableCase,   value: CamelCase }
  - { key: readability-identifier-naming.ConstexprVariablePrefix, value: k }
  - { key: cppcoreguidelines-macro-usage.AllowedRegexp,         value: '^ATLAS_' }
```
**`ConstexprVariable*` 2줄이 없으면 §3 의 `constexpr` = `kPascalCase` 규칙이 네임스페이스 스코프에서만 성립한다** (2026-08-12 실측, `architecture-design.md §15.5i`). `readability-identifier-naming` 은 식별자를 **옵션이 하나라도 설정된 가장 구체적인 수준**에서 판정하고 거기서 멈춘다. 네임스페이스 스코프 `constexpr` 은 `GlobalConstant`(위의 prefix)에서 멈춰 `kMaxPayload` 가 통과했지만, **함수 지역 `constexpr` 은 아무것도 설정되지 않아 `VariableCase: lower_case` 까지 떨어졌다** — 규칙이 지역에서만 뒤집혀 있었고, 그래서 `kSentinel` · `kPosterThreads` 가 에러였다. **설정이 컨벤션과 어긋나면 위반을 잡는 게 아니라 준수를 잡는다.**
**제외 2건은 결정이지 완화가 아니다** (2026-08-11, clang-tidy 19가 처음 실제로 채점한 런.
`architecture-design.md §15.5h`):
- `modernize-use-trailing-return-type` — 이 프로젝트는 **선행 반환형**을 쓴다(스타일 기반 Google). 이 체크는 의도적으로 고른 컨벤션을 209곳 뒤집으라고 요구하며, `modernize-*` 와일드카드에 딸려 들어왔을 뿐이다
- `bugprone-easily-swappable-parameters` — 인자 뒤바뀜이 실제로 위험한 표면(프로토콜·영속·ID)은 **§4.3 강타입 ID**가 이미 막는다
- `cppcoreguidelines-macro-usage` 는 **끄지 않았다.** `AllowedRegexp: '^ATLAS_'` 로 **§5 매크로 정책을 설정에 인코딩**했다 — 승인되지 않은 매크로는 계속 잡힌다. **끄는 것과 정책을 표현하는 것은 다르다. 가능하면 후자를 택한다**
**생성 코드는 반드시 제외한다** — `HeaderFilterRegex`로 `atlas/`만 포함하고, `ExcludeHeaderFilterRegex`로 `generated/`를 배제한다. 배제를 `HeaderFilterRegex` 하나로 처리할 수 없다 — clang-tidy가 쓰는 `llvm::Regex`는 negative lookahead를 지원하지 않는다.

**`ExcludeHeaderFilterRegex`는 clang-tidy ≥ 19를 요구한다.** 그 미만에서 벌어지는 일은 "키가 조용히 무시된다"가 **아니다** — 실측(2026-08-11, CI run 31417777347)에서 18.1.3은 `unknown key` **에러를 내고 `.clang-tidy` 전체 파싱을 포기했다**. 즉 생성 코드만 새는 것이 아니라 **네이밍·`bugprone-*`·`performance-*`·`modernize-*` 검사 집합 전체가 적용되지 않는다.** 게이트는 그래도 초록으로 보인다(그 커밋에 다른 경고가 없는 한).

**버전 고정은 `update-alternatives`로 하지 마라.** `ubuntu-24.04` 러너에는 `/usr/bin/clang-tidy`가 18 패키지의 실체 파일로 존재해서 `--install ... 100`이 링크를 교체하지 않고 조용히 진다. **모든 호출부가 `clang-tidy-19`처럼 버전 접미 바이너리를 직접 부르고, 설치 직후 버전을 로그에 찍는다.** `architecture-design.md §15.5g`.

**`.clang-format`** — 포맷을 논쟁 대상에서 완전히 제거
```yaml
BasedOnStyle: Google
ColumnLimit: 100
IndentWidth: 4
AccessModifierOffset: -4
NamespaceIndentation: None
FixNamespaceComments: true

# 레이아웃 — Google 기본을 덮어쓰는 이 프로젝트 고유 규칙
BreakBeforeBraces: Allman
SpacesInParens: Custom
SpacesInParensOptions: { InConditionalStatements: true, InCStyleCasts: false,
                         InEmptyParentheses: false, Other: true }
SpacesInAngles: Always
```
```cpp
UInt32 FrameGetLe32( std::span< const Byte > in ) noexcept
{
    for ( Int32 i = 0; i < 4; ++i )
    {
        ...
    }
}
```
**`SpacesInParens`는 clang-format ≥ 17 문법이다.** 그 미만에서는 `SpacesInParentheses: true`(불리언)만 존재하고, 빈 괄호·C 스타일 캐스트를 따로 뺄 수 없다. 로컬(VS 2022 동봉 19.1.5)과 CI(`clang-format-19`)가 모두 19이므로 이 설정을 쓴다 — 어느 한쪽이 내려가면 `.clang-format` 파싱이 통째로 실패한다(`architecture-design.md §15.5g`와 같은 함정).

**한글 주석의 폭은 바이트가 아니라 열(column)로 센다** — clang-format 19 실측: 한글 1자 = 2열. 따라서 `ColumnLimit: 100` 아래에서 **한글 주석 한 줄은 최대 48자**다. 18.1.3이 바이트로 세던 문제(`architecture-design.md §15.5f`)는 19에서 해소됐고, 로컬·CI 모두 19라 양쪽 판정이 일치한다.

**`.editorconfig`** — 개행 · 인코딩 · 들여쓰기

### 7.2 층 2 — CMake 타깃 상속

**거대 공통 헤더를 배포하지 않는다** (빌드 시간 폭탄 + 결합도). 대신 코어 타깃이 필수 설정을 `INTERFACE`/`PUBLIC`으로 전파한다:

```cmake
add_library(atlas_core ...)
target_compile_features(atlas_core PUBLIC cxx_std_20)
target_compile_definitions(atlas_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(atlas_core PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)
```

**`/utf-8`은 장식이 아니다.** `.editorconfig`가 소스를 UTF-8로 고정하지만 MSVC는 그 사실을 모르고 머신의 ANSI 코드페이지(한국어 개발기라면 949)로 디코드해 C4819를 낸다. 주석 끝의 UTF-8 선행 바이트가 개행을 삼키면 **다음 줄 코드가 주석으로 먹히는** 사고가 가능하므로, 경고 억제가 아니라 정확성 문제다. `PUBLIC`이라 코어를 링크하는 모든 타깃이 함께 받는다.

`target_link_libraries(atlas_world PRIVATE atlas_core)` 한 줄이면 **모든 서버가 같은 표준 · 경고 · 정의를 자동 상속**한다. 새 서버를 추가해도 설정을 잊을 수가 없다.

### 7.3 층 3 — CI 게이트 + pre-commit

```
format-check → clang-tidy → build(unity ON) → build(unity OFF) → test
```

- **게이트가 없으면 clang-tidy도 돌지 않고 문서는 장식이 된다**
- **non-unity 빌드가 "누락 include / ODR 충돌" 게이트를 겸한다** (설계 문서 §15.1 대가 4번). 이 게이트가 없으면 반드시 터진다
- **pre-commit hook**: 변경 파일만 format + tidy. CI보다 먼저 잡아 왕복 비용을 줄인다

#### `clang-tidy`는 CI(Linux/clang) 전용이다 — 로컬 Windows 게이트에서는 건너뛴다

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

- **그래서 `--extra-arg=/Y-`를 끼워 넣지 않는다.** 그렇게 하면 **컴파일러가 실제로 빌드하지 않는
  translation unit**을 검사하게 된다 — PCH가 주입하던 것과 다른 전처리 상태를 보므로, 통과해도
  실제 빌드를 보증하지 못하고 반대로 없는 문제를 신고한다. 게이트가 거짓 신호를 주는 쪽이
  게이트가 없는 것보다 나쁘다.
- **PCH를 끄는 것도 답이 아니다.** PCH는 §15.1에서 빌드 시간을 위해 명시적으로 선택한 것이고,
  린터 하나 때문에 되돌릴 대상이 아니다.
- **결정: 강제 지점을 `linux-ci`로 옮긴다.** `.github/workflows/ci.yml`의 clang-tidy 단계는
  clang이 만든 compile db와 clang이 읽을 수 있는 PCH를 쓰므로 그대로 성립한다.
  `server/scripts/ci-gate.ps1`은 이 단계를 **사유를 출력하고** 건너뛴다 — 조용한 스킵은 게이트가
  죽은 것을 감춘다.
- **대가(감수한다)**: §7.1의 네이밍 · `bugprone-*` · `modernize-*` 강제는 로컬 게이트에서 돌지
  않는다. 로컬 게이트에 남는 기계 강제는 `.clang-format`(층 1) + §7.2 경고 옵션 + unity ON/OFF
  두 빌드다.
- **사전 필터는 있다, 그러나 게이트가 아니다** (2026-08-13): `server\scripts\tidy-prefilter.ps1`
  이 `compile_commands.json` **사본**에서 PCH 플래그만 벗겨 로컬 clang-tidy 를 돌린다(48 TU ·
  18분, `-Filter` 로 파일 단위). **항상 exit 0** 이다 — 게이트가 아니라는 것을 종료 코드로도
  말한다. 두 툴체인의 발산은 **양방향**이다: MSVC STL 은 `bugprone-unchecked-optional-access`
  를 아예 보고하지 않고(CI 만 보는 축), 반대로 `#if defined(_WIN32)` 분기는 리눅스 CI 가 컴파일
  조차 하지 않는다(로컬만 보는 축). 근거와 실측은 설계 문서 §15.5i.
- **해소 조건**: 로컬에 clang-cl 도입. clang-cl 프리셋은 clang이 읽는 PCH를 만들므로 그 시점에
  이 단계를 `ci-gate.ps1`으로 되돌린다.

#### `NOLINT` 은 **진단이 찍히는 줄**에 붙는다 — 중괄호 줄이 아니다

`// NOLINT` 은 **같은 줄**의 진단만 억제한다. §7.1 의 Allman 은 여는 중괄호를 **다음 줄**로
내리므로, 중괄호에 꼬리 주석을 붙이면 억제가 한 줄 어긋난다. clang-tidy 는 `catch` 키워드나
함수 시그니처 줄에 진단을 찍지, 중괄호 줄에 찍지 않는다.

```cpp
// 틀림 - NOLINT 이 중괄호 줄에 있고 진단은 catch 줄에 찍힌다
catch ( ... )
{  // NOLINT - 더 보고할 곳이 없음
}

// 맞음
catch ( ... )  // NOLINT - 더 보고할 곳이 없음
{
}
```

여러 줄에 걸친 구문이면 `// NOLINTNEXTLINE(check-name)` 을 진단 줄 **바로 위**에 둔다.

**이 규칙은 실제 사고에서 나왔다.** K&R 시절의 `} catch (...) {  // NOLINT` 은 진단 줄과 중괄호가
같은 줄이라 옳았고, Allman 전환이 그 19곳을 전부 조용히 무효로 만들었다. 로컬 게이트는
`clang-tidy` 를 건너뛰므로(위 문단) 아무도 눈치채지 못했고, CI 가
`bugprone-empty-catch` 12건으로 잡았다(설계 문서 §15.5l). **레이아웃 규칙을 바꾸는 변경은
꼬리 주석에 의존하는 도구 지시자를 같이 옮겼는지 확인한다.**

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
server/atlas/config/secret_config.h   /  .cpp   (.env 시크릿 — 값은 로그·예외에 싣지 않는다)
server/server.ini                      (커밋되는 런타임 설정. 설계 문서 §5.4)
server/atlas/net/net_types.h           (asio 별칭)
server/atlas/net/io_runner.h  /  .cpp  (io_context + 워커 풀 + graceful stop)
server/atlas/net/session.h    /  .cpp  (strand 세션 — 바이트 스트림까지, 프레이밍 없음)
server/atlas/net/acceptor.h   /  .cpp  (accept 루프 + 세션 생성)
server/atlas/proto/crc32.h    /  .cpp  (CRC-32 — 프레이밍 무결성 전용, 변조 방지 아님)
server/atlas/proto/frame.h    /  .cpp  (12바이트 고정 헤더 인코딩/디코딩 — 설계 문서 §8.1)
server/atlas/proto/frame_reader.h    /  .cpp  (스트림 재조립 상태 기계 — throw 하지 않는다)
server/atlas/proto/session_framing.h /  .cpp  (Session ↔ FrameReader 결선)
```

**net 3종이 목록에 있는 이유는 "게임이 바뀌어도 안 바뀌기 때문"이다.** 이 파일들의 경계는
**바이트 스트림까지**이며 프레임 · 페이로드 식별자 · 무결성 필드를 일절 모른다(설계 문서 §8은 별도
계층). 게임이 프로토콜을 바꿔도 이 3종은 그대로 따라간다 — 그 경계를 무너뜨리는 순간 자산 목록에서
빠져야 하는 파일이 된다.

**proto 4종이 목록에 있는 근거는 같지만 조건이 하나 더 붙는다.** 프레임 계층은 opcode 를
`UInt16` 로만 다루고 **그 의미를 모른다** — opcode → 핸들러 매핑을 여기에 넣는 순간 게임 코드가
되어 목록에서 빠진다. 의존 방향은 `atlas_proto → atlas_net` 고정이며(설계 문서 §8.1), 역방향
링크는 위의 바이트 경계를 컴파일 타임 사실에서 주석으로 격하시킨다.

경로에 `server/` 접두가 붙는 이유는 CMake 루트이자 include 루트가 `server/`이기 때문이다 —
include 지점은 `#include "atlas/core/types.h"`로 쓰지만 파일은 `server/atlas/...`에 있다.

이 목록이 따라가므로 **"게임마다 스타일이 갈리는" 상황이 구조적으로 불가능해진다.**

---

## 9. 주석 규칙

**주석은 코드가 말하지 못하는 것만 적는다.** "무엇을 하는가"는 코드가 이미 말한다. 주석의 몫은
**"왜 이렇게 했는가"**와 **"이렇게 하지 않으면 무엇이 깨지는가"** 둘뿐이다.

### 9.1 언어와 어미

- **한글**로 쓴다. 식별자 · 타입명 · 기술 용어는 원문 그대로 둔다(`strand`, `io_context`,
  `Session`, `shared_ptr`). 억지 번역이 오히려 검색을 막는다.
- **체언 종결.** `~습니다` · `~이다` · `~한다` 를 쓰지 않는다. 동사가 꼭 필요하면
  `~함` · `~됨` · `~안 됨` · `~할 것`.

```cpp
// 좋음
// 정수형 벡터
// 쓰기 큐 상한. 초과 시 연결 종료
// 락 없음. 모든 멤버는 strand 위에서만 접근

// 나쁨
// 이것은 정수형 벡터입니다
// 큐가 가득 차면 연결을 종료한다
```

- **금지 문자**: 이모지 전체, 박스 문자(`─ │ ┌`), `—`, `→`, `·`, `§`.
  ASCII `= - : ->` 로 대체한다. 한글 외의 비ASCII는 주석에 넣지 않는다 —
  clang-format 버전 간 폭 계산 차이가 게이트를 갈라놓은 전례가 있다(`architecture-design.md §15.5f`).

### 9.2 3계층

**① 파일 최상단 요약** — 파일당 정확히 1개, 1~2줄.
헤더는 `#pragma once` 아래 · `#include` 위, `.cpp` 는 파일 맨 위 · `#include` 위.

```cpp
#pragma once

// =============================================================================
// TCP 연결 1개 = Session 1개. 경계는 바이트 스트림
// 프레임과 프로토콜은 atlas/proto 가 밖에서 주입한다
// =============================================================================

#include <array>
```

**② 섹션 배너** — 파일 안의 논리 구획. 배너 폭은 **80칸 고정**(`// ` + `=` 77개).
제목 한 줄만 담고, 설명이 필요하면 배너 아래에 블록 주석으로 붙인다.

```cpp
// =============================================================================
// 세션 수명 주기
// =============================================================================
```

**파일당 0~4개.** 함수마다 달면 배너가 노이즈가 되어 구획을 나누는 기능을 잃는다.

**③ 블록 주석** — 선언(클래스 · 함수 · 상수) 바로 위. **원칙 3줄, 상한 5줄.**
그 이상 필요한 설명은 주석이 아니라 설계 문서로 가야 할 내용이다.

```cpp
// 유휴 타임아웃 300초
// 전원이 끊긴 피어는 FIN 을 보내지 않는다
// 타이머가 없으면 그 세션과 버퍼는 영원히 남는다
static constexpr Duration kDefaultIdleTimeout = Seconds{ 300 };
```

한 줄로 끝나는 내용은 블록으로 올리지 말고 ④로 내린다.

**④ 트레일링 주석** — 코드 우측. 코드와 `//` 사이 공백 2칸.

```cpp
UInt32 length_;      // 페이로드 바이트 수
UInt16 packet_id_;   // 패킷 종류
Byte   flags_;       // 예약
```

- **연속된 트레일링 주석의 정렬은 손으로 맞추지 않는다.** `AlignTrailingComments`(Google 기본
  ON)가 clang-format 단계에서 가장 긴 줄에 맞춰 자동 정렬한다.
- 트레일링 주석에는 ①②의 배너 규칙을 적용하지 않는다.

### 9.3 줄바꿈 — 한 줄은 한 문장

**문장 중간에서 줄을 바꾸지 않는다.** 주석 한 줄은 그 자체로 읽히는 완결된 단위여야 한다.

```cpp
// 나쁨 - 두 줄 모두 혼자서는 읽히지 않는다
// [AD 9.2] 원장 교차 지점. Ctx 는 값으로 건넴 - 참조였다면 호출자의 CtxScope 가
// I/O 스레드에서 풀리는 순간 dangling 이 되고, 그것이 9.2 가 경고하는 덫

// 좋음 - 각 줄이 한 문장
// [AD 9.2] 원장 교차 지점. Ctx 는 값으로 건넨다
// 참조면 호출자의 CtxScope 가 풀리는 순간 dangling 이 된다
// guard 가 사본을 실제 실행 지점에 다시 설치한다
```

**그러려면 문장을 짧게 쓴다.** 한글 48자(= `ColumnLimit: 100`, 한글 1자 = 2열, §7.1 실측)를
넘길 것 같으면 **문장을 쪼갠다.** 이 규칙은 취향이 아니라 도구의 제약이다: 48자를 넘기는 순간
`ReflowComments` 가 문장 한가운데를 접어 버리고, 그 결과는 손으로 되돌릴 수 없다.

한 문장 안의 부연은 줄을 넘기지 말고 접속을 끊어 다음 문장으로 만든다. "~이고" · "~이며" ·
"~해서" 로 이어붙이다 줄이 넘치는 것이 위 나쁜 예가 생기는 유일한 경로다.

### 9.3a 분량

- 파일당 주석 비율 목표 **15% 이하**.
- 지울 때의 우선순위: **코드 반복 > 자명한 설명 > 장황한 서술 > 설계 근거**.
  맨 오른쪽(설계 근거)은 남긴다 — 그것이 주석이 존재하는 유일한 이유다.

### 9.4 설계 문서 참조

형식은 `[AD 9.1]` · `[CS 4.3]` 로 고정하고 줄 맨 앞에 둔다. 문장 안에 녹이지 않는다.

```cpp
// [AD 9.1] 동시성 모델은 strand 하나뿐. 락을 여기 넣으면 모델이 둘이 되고
// 교착은 정확히 그 교차점에서 발생
```

### 9.5 그 밖

- `/* */` 를 쓰지 않는다. `//` 만 쓴다 — 블록 주석은 중첩되지 않아 주석 처리 사고를 낸다.
- **주석 처리된 죽은 코드를 남기지 않는다.** 지운다. 복원은 `git`의 일이다.
- `// TODO:` · `// FIXME:` 만 허용. 이름과 날짜는 적지 않는다 — `git blame` 이 답한다.
- Doxygen(`/** @brief */`)은 **도입하지 않는다.** 문서를 생성하지 않는 프로젝트에서 태그는
  분량만 늘린다.
