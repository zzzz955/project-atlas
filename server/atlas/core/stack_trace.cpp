#include "atlas/core/stack_trace.h"

#include <boost/stacktrace/stacktrace.hpp>
#include <cstdint>
#include <format>
#include <tuple>

#if defined(_WIN32)
#include <Windows.h>

#include <filesystem>
#include <mutex>
#endif

namespace atlas {
namespace {

constexpr std::size_t kMaxStackFrames = 64;

#if defined(_WIN32)
void EnsureLocalSymbolPath() noexcept {
    static std::once_flag once;
    try {
        std::call_once(once, [] {
            // Respect an operator-provided symbol store. Otherwise make the executable directory
            // discoverable so a local PDB beside the binary yields file:line immediately.
            if (::GetEnvironmentVariableW(L"_NT_SYMBOL_PATH", nullptr, 0) != 0) {
                return;
            }

            std::wstring executable_path(32'768, L'\0');
            const DWORD length = ::GetModuleFileNameW(nullptr, executable_path.data(),
                                                      static_cast<DWORD>(executable_path.size()));
            if (length == 0 || static_cast<std::size_t>(length) >= executable_path.size()) {
                return;
            }
            executable_path.resize(length);
            const std::wstring directory =
                std::filesystem::path(executable_path).parent_path().wstring();
            std::ignore = ::SetEnvironmentVariableW(L"_NT_SYMBOL_PATH", directory.c_str());
        });
    } catch (...) {  // NOLINT — diagnostics must never replace the original failure.
    }
}
#else
void EnsureLocalSymbolPath() noexcept {}
#endif

std::string FormatStackTrace(const boost::stacktrace::stacktrace& trace) {
    if (trace.empty()) {
        return {};
    }

    std::string output;
    for (std::size_t index = 0; index < trace.size(); ++index) {
        const auto address = reinterpret_cast<std::uintptr_t>(trace[index].address());
        output += std::format("{:2}# {:#x} {}\n", index, address,
                              boost::stacktrace::to_string(trace[index]));
    }
    return output;
}

}  // namespace

std::string CaptureStackTrace(std::size_t skip) noexcept {
    try {
        EnsureLocalSymbolPath();
        // One frame for this function itself. Callers only count frames above this API.
        return FormatStackTrace(boost::stacktrace::stacktrace(skip + 1, kMaxStackFrames));
    } catch (...) {  // NOLINT — diagnostics must never replace the original failure.
        return {};
    }
}

std::string CaptureCurrentExceptionStackTrace() noexcept {
    try {
        EnsureLocalSymbolPath();
        return FormatStackTrace(boost::stacktrace::stacktrace::from_current_exception());
    } catch (...) {  // NOLINT — see CaptureStackTrace.
        return {};
    }
}

}  // namespace atlas
