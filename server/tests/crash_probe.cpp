#include <csignal>
#include <filesystem>
#include <string>

#include "atlas/core/log.h"

#if defined(_WIN32)
#include <Windows.h>
#endif

// Deliberately crashes a child process so the dump pipeline can be verified without taking a test
// runner or the server down. It is not registered with ctest: Linux core placement is host policy,
// and a machine with RLIMIT_CORE=0 must not turn the normal gate red. See §11.1 for the manual
// verification commands.
int main(int argc, char** argv) {  // NOLINT — the standard fixes main's signature.
    const std::filesystem::path directory =
        argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("crash-probe");

    atlas::LogConfig config;
    config.directory = directory.string();
    config.basename = "atlas_crash_probe";
    config.console = false;
    atlas::LogInit(config);
    ATLAS_LOG_FATAL("intentional crash probe");
    atlas::LogFlush();

#if defined(_WIN32)
    ::SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    constexpr DWORD kProbeExceptionCode = 0xE0425052;
    ::RaiseException(kProbeExceptionCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
    std::raise(SIGSEGV);
#endif
    return 1;
}
