#pragma once

// =============================================================================
// [AD 11.1] 심볼 붙은 스택 트레이스
// 무거운 Boost.Stacktrace 는 out-of-line 뒤에 가둔다
// core 소비자가 구현 헤더를 파싱하지 않게 하려는 것
// =============================================================================

#include <cstddef>
#include <string>

namespace atlas
{

// skip 은 이 API 위쪽 호출자 수. 실패는 빈 문자열로 돌려줌
// 진단이 원래 실패를 다른 예외로 덮어쓰면 안 됨
[[nodiscard]] std::string CaptureStackTrace( std::size_t skip = 0 ) noexcept;

// catch 블록 안에서 호출. 언와인딩으로 사라진 throw 지점을 보존
[[nodiscard]] std::string CaptureCurrentExceptionStackTrace() noexcept;

}  // namespace atlas
