// =============================================================================
// 덤프 파이프라인 검증용 의도적 크래시. 러너나 서버를 죽이지 않도록 별도 프로세스
// [AD 11.1] ctest 미등록. RLIMIT_CORE=0 머신에서 게이트가 붉어지면 안 됨
// =============================================================================

#include <csignal>
#include <filesystem>
#include <string>

#include "atlas/core/log.h"

#if defined( _WIN32 )
#include <Windows.h>
#endif

int main( int argc, char** argv )
{  // NOLINT: main 시그니처는 표준이 고정
    const std::filesystem::path directory =
        argc > 1 ? std::filesystem::path( argv[1] ) : std::filesystem::path( "crash-probe" );

    atlas::LogConfig config;
    config.directory = directory.string();
    config.basename = "atlas_crash_probe";
    config.console = false;
    atlas::LogInit( config );
    ATLAS_LOG_FATAL( "intentional crash probe" );
    atlas::LogFlush();

#if defined( _WIN32 )
    ::SetErrorMode( SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS );
    constexpr DWORD kProbeExceptionCode = 0xE0425052;
    ::RaiseException( kProbeExceptionCode, EXCEPTION_NONCONTINUABLE, 0, nullptr );
#else
    std::raise( SIGSEGV );
#endif
    return 1;
}
