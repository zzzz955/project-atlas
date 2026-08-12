#pragma once
#include <string>
#include <string_view>

// architecture-design.md §11.1 — fatal crash diagnostics.

namespace atlas {

struct CrashDiagnosticsConfig {
    // Windows minidumps are written here. Linux core placement is controlled by the host's
    // core_pattern; the process enables RLIMIT_CORE and then lets the kernel create the core.
    std::string directory;
    std::string basename{"atlas"};
};

// Process-wide lifecycle. LogInit/LogShutdown own it so the diagnostic log and dump use the same
// basename and lifetime. Not safe to call concurrently.
void CrashDiagnosticsInit(const CrashDiagnosticsConfig& config);
void CrashDiagnosticsShutdown() noexcept;

[[nodiscard]] std::string_view BuildId() noexcept;

}  // namespace atlas
