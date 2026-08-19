#pragma once

// =============================================================================
// [AD 11.1] 치명 크래시 진단. 미니덤프 - 코어 덤프 - terminate 훅 설치
// =============================================================================

#include <string>
#include <string_view>

namespace atlas
{

struct CrashDiagnosticsConfig
{
    // Windows 미니덤프 경로. Linux 코어 위치는 호스트 core_pattern 이 결정
    std::string directory;
    std::string basename{ "atlas" };
};

// LogInit/LogShutdown 이 소유해 로그와 덤프가 같은 basename 을 씀. 동시 호출 안 됨
void CrashDiagnosticsInit( const CrashDiagnosticsConfig& config );
void CrashDiagnosticsShutdown() noexcept;

[[nodiscard]] std::string_view BuildId() noexcept;

}  // namespace atlas
