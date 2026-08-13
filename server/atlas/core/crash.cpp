#include "atlas/core/crash.h"

#include <array>
#include <csignal>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <string>
#include <tuple>

#include "atlas/core/error.h"
#include "atlas/core/log.h"
#include "atlas/core/stack_trace.h"

#if defined(_WIN32)
// DbgHelp uses Windows types but does not include their declarations itself. Keep these in
// dependency order instead of alphabetizing DbgHelp ahead of Windows.h.
// clang-format off
#include <Windows.h>
#include <DbgHelp.h>
// clang-format on
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#ifndef ATLAS_BUILD_ID
#define ATLAS_BUILD_ID "dev"
#endif

namespace atlas {
namespace {

std::terminate_handler g_previous_terminate = nullptr;
bool g_initialized = false;

[[noreturn]] void TerminateHandler() noexcept {
    const std::string trace = CaptureCurrentExceptionStackTrace();
    if (trace.empty()) {
        ATLAS_LOG_FATAL("std::terminate called [build={}]", BuildId());
    } else {
        ATLAS_LOG_FATAL("std::terminate called [build={}]\nthrow stack:\n{}", BuildId(), trace);
    }
    // This is normal execution context, not a signal handler. Flushing here is safe and gives the
    // platform crash path below a chance to preserve the log tail before it creates the dump.
    LogFlush();

#if defined(_WIN32)
    // An unhandled SEH exception reaches the top-level filter with a valid EXCEPTION_POINTERS,
    // which is what MiniDumpWriteDump needs to identify the faulting thread and instruction.
    constexpr DWORD kAtlasTerminateCode = 0xE0424C53;
    ::RaiseException(kAtlasTerminateCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#endif
    std::abort();
}

#if defined(_WIN32)

struct WindowsCrashState {
    HANDLE request_event{nullptr};
    HANDLE completed_event{nullptr};
    HANDLE dump_thread{nullptr};
    EXCEPTION_POINTERS* exception{nullptr};
    DWORD crashing_thread_id{0};
    volatile LONG dump_started{0};
    volatile LONG stop_requested{0};
    std::filesystem::path directory;
    std::wstring basename;
    LPTOP_LEVEL_EXCEPTION_FILTER previous_filter{nullptr};
};

WindowsCrashState g_windows_crash;

std::filesystem::path WindowsDumpPath() {
    SYSTEMTIME now{};
    ::GetSystemTime(&now);
    const DWORD process_id = ::GetCurrentProcessId();

    std::array<wchar_t, 256> filename{};
    std::swprintf(filename.data(), filename.size(), L"%ls-%04u%02u%02uT%02u%02u%02uZ-p%lu-%hs.dmp",
                  g_windows_crash.basename.c_str(), now.wYear, now.wMonth, now.wDay, now.wHour,
                  now.wMinute, now.wSecond, static_cast<unsigned long>(process_id), ATLAS_BUILD_ID);
    return g_windows_crash.directory / filename.data();
}

DWORD WINAPI DumpThread(void*) {
    for (;;) {
        if (::WaitForSingleObject(g_windows_crash.request_event, INFINITE) != WAIT_OBJECT_0) {
            return 1;
        }
        if (::InterlockedCompareExchange(&g_windows_crash.stop_requested, 0, 0) != 0) {
            return 0;
        }

        const std::filesystem::path path = WindowsDumpPath();
        const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION exception_info{};
            exception_info.ThreadId = g_windows_crash.crashing_thread_id;
            exception_info.ExceptionPointers = g_windows_crash.exception;
            exception_info.ClientPointers = FALSE;

            // 🔴 `MINIDUMP_TYPE` is a FLAG enum and `MiniDumpWriteDump` documents the OR as its
            // calling contract, so the combined value is correct by design and is deliberately not
            // one of the enumerators. The analyzer cannot tell a flag enum from a plain one; this
            // is the same judgement architecture-design.md §15.5h recorded for the other
            // out-of-range enum cast in the test tree. The marker below covers one line only and
            // must therefore stay against the code, never on a comment line (§15.5i).
            // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
            constexpr auto kDumpType = static_cast<MINIDUMP_TYPE>(
                MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
            const BOOL dump_written =
                ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), file, kDumpType,
                                    &exception_info, nullptr, nullptr);
            std::ignore = ::CloseHandle(file);
            if (dump_written == FALSE) {
                // A partial file looks like a valid incident artifact but can never be opened.
                std::ignore = ::DeleteFileW(path.c_str());
            }
        }
        std::ignore = ::SetEvent(g_windows_crash.completed_event);
    }
}

LONG WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS* exception) {
    if (::InterlockedCompareExchange(&g_windows_crash.dump_started, 1, 0) == 0) {
        g_windows_crash.exception = exception;
        g_windows_crash.crashing_thread_id = ::GetCurrentThreadId();
        std::ignore = ::ResetEvent(g_windows_crash.completed_event);
        std::ignore = ::SetEvent(g_windows_crash.request_event);
        // Do not let a damaged dump path keep an already-failing process alive forever.
        std::ignore = ::WaitForSingleObject(g_windows_crash.completed_event, 10'000);
    } else {
        // A second crashing thread must not terminate the process while the first dump is being
        // written. It observes the same bounded wait and then lets Windows continue crash handling.
        std::ignore = ::WaitForSingleObject(g_windows_crash.completed_event, 10'000);
    }

    if (g_windows_crash.previous_filter != nullptr &&
        g_windows_crash.previous_filter != &UnhandledExceptionFilter) {
        return g_windows_crash.previous_filter(exception);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InitPlatformCrashDiagnostics(const CrashDiagnosticsConfig& config) {
    g_windows_crash.directory = std::filesystem::path(config.directory);
    g_windows_crash.basename = std::filesystem::path(config.basename).wstring();
    std::filesystem::create_directories(g_windows_crash.directory);

    g_windows_crash.request_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ATLAS_CHECK(g_windows_crash.request_event != nullptr,
                "failed to create the Windows crash-dump request event");

    g_windows_crash.completed_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_windows_crash.completed_event == nullptr) {
        std::ignore = ::CloseHandle(g_windows_crash.request_event);
        g_windows_crash.request_event = nullptr;
        ATLAS_CHECK(false, "failed to create the Windows crash-dump completion event");
    }

    g_windows_crash.dump_thread = ::CreateThread(nullptr, 0, &DumpThread, nullptr, 0, nullptr);
    if (g_windows_crash.dump_thread == nullptr) {
        std::ignore = ::CloseHandle(g_windows_crash.request_event);
        std::ignore = ::CloseHandle(g_windows_crash.completed_event);
        g_windows_crash.request_event = nullptr;
        g_windows_crash.completed_event = nullptr;
        ATLAS_CHECK(false, "failed to create the Windows crash-dump thread");
    }
    g_windows_crash.previous_filter = ::SetUnhandledExceptionFilter(&UnhandledExceptionFilter);
}

void ShutdownPlatformCrashDiagnostics() noexcept {
    std::ignore = ::SetUnhandledExceptionFilter(g_windows_crash.previous_filter);
    if (g_windows_crash.dump_thread != nullptr) {
        std::ignore = ::InterlockedExchange(&g_windows_crash.stop_requested, 1);
        std::ignore = ::SetEvent(g_windows_crash.request_event);
        std::ignore = ::WaitForSingleObject(g_windows_crash.dump_thread, 2'000);
        std::ignore = ::CloseHandle(g_windows_crash.dump_thread);
    }
    if (g_windows_crash.request_event != nullptr) {
        std::ignore = ::CloseHandle(g_windows_crash.request_event);
    }
    if (g_windows_crash.completed_event != nullptr) {
        std::ignore = ::CloseHandle(g_windows_crash.completed_event);
    }
    g_windows_crash = {};
}

#else

constexpr std::array<int, 5> kFatalSignals{SIGABRT, SIGSEGV, SIGILL, SIGFPE, SIGBUS};
std::array<struct sigaction, kFatalSignals.size()> g_previous_actions{};

extern "C" void FatalSignalHandler(int signal_number) {
    // Only async-signal-safe calls are allowed here. Symbolization, allocation, spdlog and even the
    // C++ filesystem are deliberately absent; the kernel core preserves the crashing thread and
    // the offline symbolizer joins it with the exact DWARF artifact.
    struct sigaction default_action{};
    default_action.sa_handler = SIG_DFL;
    static_cast<void>(::sigemptyset(&default_action.sa_mask));
    static_cast<void>(::sigaction(signal_number, &default_action, nullptr));

    // The delivered signal is blocked for the duration of its handler. Unblock it before re-send;
    // otherwise kill() only queues it and the _exit below would bypass the kernel core path.
    sigset_t unblock_set{};
    static_cast<void>(::sigemptyset(&unblock_set));
    static_cast<void>(::sigaddset(&unblock_set, signal_number));
    static_cast<void>(::sigprocmask(SIG_UNBLOCK, &unblock_set, nullptr));
    static_cast<void>(::kill(::getpid(), signal_number));
    ::_exit(128 + signal_number);
}

void InitPlatformCrashDiagnostics(const CrashDiagnosticsConfig&) {
    struct rlimit core_limit{};
    if (::getrlimit(RLIMIT_CORE, &core_limit) == 0 && core_limit.rlim_cur < core_limit.rlim_max) {
        core_limit.rlim_cur = core_limit.rlim_max;
        if (::setrlimit(RLIMIT_CORE, &core_limit) != 0) {
            ATLAS_LOG_WARN(
                "failed to raise RLIMIT_CORE; Linux may not create a core dump [build={}]",
                BuildId());
        }
    }

    for (std::size_t index = 0; index < kFatalSignals.size(); ++index) {
        struct sigaction action{};
        action.sa_handler = &FatalSignalHandler;
        std::ignore = ::sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESETHAND;
        if (::sigaction(kFatalSignals[index], &action, &g_previous_actions[index]) != 0) {
            for (std::size_t installed = 0; installed < index; ++installed) {
                std::ignore =
                    ::sigaction(kFatalSignals[installed], &g_previous_actions[installed], nullptr);
            }
            ATLAS_CHECK(false, "failed to install crash handler for signal {}",
                        kFatalSignals[index]);
        }
    }
}

void ShutdownPlatformCrashDiagnostics() noexcept {
    for (std::size_t index = 0; index < kFatalSignals.size(); ++index) {
        std::ignore = ::sigaction(kFatalSignals[index], &g_previous_actions[index], nullptr);
    }
}

#endif

}  // namespace

void CrashDiagnosticsInit(const CrashDiagnosticsConfig& config) {
    CrashDiagnosticsShutdown();
    ATLAS_CHECK(!config.directory.empty(), "crash dump directory must not be empty");
    ATLAS_CHECK(!config.basename.empty(), "crash dump basename must not be empty");

    InitPlatformCrashDiagnostics(config);
    // DbgEng performs noticeable one-time initialization on Windows. Pay it during process start,
    // before an exception path can stall an I/O worker that happens to be the first caller.
    std::ignore = CaptureStackTrace(1);
    g_previous_terminate = std::set_terminate(&TerminateHandler);
    g_initialized = true;
    ATLAS_LOG_INFO("crash diagnostics ready [build={} directory={}]", BuildId(), config.directory);
}

void CrashDiagnosticsShutdown() noexcept {
    if (!g_initialized) {
        return;
    }
    std::set_terminate(g_previous_terminate);
    ShutdownPlatformCrashDiagnostics();
    g_previous_terminate = nullptr;
    g_initialized = false;
}

std::string_view BuildId() noexcept { return ATLAS_BUILD_ID; }

}  // namespace atlas
