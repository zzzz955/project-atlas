// =============================================================================
// Boost.Stacktrace 캡처와 포맷. 어떤 경로도 throw 하지 않음
// =============================================================================

#include "atlas/core/stack_trace.h"

#include <boost/stacktrace/stacktrace.hpp>
#include <cstdint>
#include <format>
#include <tuple>

#if defined( _WIN32 )
#include <Windows.h>

#include <filesystem>
#include <mutex>
#endif

namespace atlas
{
namespace
{

constexpr std::size_t kMaxStackFrames = 64;

#if defined( _WIN32 )
void EnsureLocalSymbolPath() noexcept
{
    static std::once_flag once;
    try
    {
        std::call_once(
            once,
            []
            {
                // 운영자 지정 심볼 스토어 우선
                // 없으면 실행 파일 디렉터리를 등록한다
                // 옆의 PDB 로 file:line 이 바로 나온다
                if ( ::GetEnvironmentVariableW( L"_NT_SYMBOL_PATH", nullptr, 0 ) != 0 )
                {
                    return;
                }

                std::wstring executable_path( 32'768, L'\0' );
                const DWORD length =
                    ::GetModuleFileNameW( nullptr, executable_path.data(),
                                          static_cast< DWORD >( executable_path.size() ) );
                if ( length == 0 || static_cast< std::size_t >( length ) >= executable_path.size() )
                {
                    return;
                }
                executable_path.resize( length );
                const std::wstring directory =
                    std::filesystem::path( executable_path ).parent_path().wstring();
                std::ignore = ::SetEnvironmentVariableW( L"_NT_SYMBOL_PATH", directory.c_str() );
            } );
    }
    catch ( ... )  // NOLINT - 진단이 원래 실패를 덮어쓰면 안 됨
    {
    }
}
#else
void EnsureLocalSymbolPath() noexcept {}
#endif

std::string FormatStackTrace( const boost::stacktrace::stacktrace& trace )
{
    if ( trace.empty() )
    {
        return {};
    }

    std::string output;
    for ( std::size_t index = 0; index < trace.size(); ++index )
    {
        const auto address = reinterpret_cast< std::uintptr_t >( trace[index].address() );
        output += std::format( "{:2}# {:#x} {}\n", index, address,
                               boost::stacktrace::to_string( trace[index] ) );
    }
    return output;
}

}  // namespace

std::string CaptureStackTrace( std::size_t skip ) noexcept
{
    try
    {
        EnsureLocalSymbolPath();
        // 이 함수 자신의 프레임 1개. 호출자는 이 API 위쪽만 셈
        return FormatStackTrace( boost::stacktrace::stacktrace( skip + 1, kMaxStackFrames ) );
    }
    catch ( ... )  // NOLINT - 진단이 원래 실패를 덮어쓰면 안 됨
    {
        return {};
    }
}

std::string CaptureCurrentExceptionStackTrace() noexcept
{
    try
    {
        EnsureLocalSymbolPath();
        return FormatStackTrace( boost::stacktrace::stacktrace::from_current_exception() );
    }
    catch ( ... )  // NOLINT - CaptureStackTrace 참고
    {
        return {};
    }
}

}  // namespace atlas
