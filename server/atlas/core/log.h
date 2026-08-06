#pragma once
#include <atomic>
#include <format>
#include <source_location>
#include <string>
#include <string_view>

#include "atlas/core/types.h"

// architecture-design.md §11.1 — logging.
//
// 🔴 spdlog is NOT included here. Every translation unit in the server includes this header, and
// spdlog + fmt is one of the heaviest header sets in the dependency list; parsing it everywhere
// would hand back the build time that the PCH and the unity build bought (§15.1). The whole spdlog
// surface lives behind the out-of-line `LogWrite` in log.cpp, and the call site formats with
// `std::format` instead. That is the pimpl of this layer.

namespace atlas {

// Ordered by severity so the level gate is a single integer comparison.
enum class LogLevel : UInt8 { Trace = 0, Debug, Info, Warn, Error, Fatal, Off };

struct LogConfig {
    // Empty directory => no file sink (tests, and anything that only wants the console).
    std::string directory{};
    std::string basename{"atlas"};
    LogLevel level{LogLevel::Info};
    // Records at or above this level are flushed immediately instead of waiting for the drain.
    LogLevel flush_level{LogLevel::Warn};
    // Daily rolling retention, in files == days. See architecture-design.md §11.1.
    UInt16 retain_days{14};
    bool console{true};
};

// 🔴 Not thread-safe against concurrent logging: call once at start-up and once at shutdown, from
// the thread that owns the process lifetime. Re-calling LogInit tears the previous logger down
// first, so it is idempotent in the sense that matters.
void LogInit(const LogConfig& config);
void LogShutdown() noexcept;
void LogFlush() noexcept;

void SetLogLevel(LogLevel level) noexcept;
LogLevel CurrentLogLevel() noexcept;

// Records emitted at `level` since process start. This is what makes "the guard logged exactly one
// ERROR" a testable statement instead of an assertion in a comment; it doubles as a cheap ops
// counter for error-rate alarms.
UInt64 LogCount(LogLevel level) noexcept;

namespace detail {

// Inline variable rather than a function-local static: the level gate sits in front of every log
// call site, and a function-local static would pay a thread-safe-init guard check every time.
inline std::atomic<UInt8> g_log_level{static_cast<UInt8>(LogLevel::Info)};

}  // namespace detail

// 🔴 The level check happens BEFORE any formatting, so a disabled level builds zero strings.
[[nodiscard]] inline bool LogEnabled(LogLevel level) noexcept {
    return static_cast<UInt8>(level) >= detail::g_log_level.load(std::memory_order_relaxed);
}

// `loc` is a defaulted parameter, not a macro capture: `std::source_location::current()` is
// evaluated at the call site, which is exactly where the macro expands. cpp-style.md §5 — this is
// why the macros do not need __FILE__/__LINE__.
void LogWrite(LogLevel level, std::string_view message,
              std::source_location loc = std::source_location::current()) noexcept;

}  // namespace atlas

// 🔴 cpp-style.md §5 — the ONLY justification for these being macros is lazy evaluation.
//
//     ATLAS_LOG_DEBUG("dump={}", ExpensiveDump(a));
//
// with DEBUG disabled must not call ExpensiveDump() at all. A function cannot do that: its
// arguments are evaluated before the call. There is deliberately no shared ATLAS_LOG_IMPL helper —
// the macro whitelist is exactly these six names plus the three in core/error.h, and a helper macro
// would be a tenth name outside it. The repetition is the price of a checkable whitelist.

#if defined(NDEBUG)
// The second thing only a macro can do: delete TRACE/DEBUG from a release binary outright, so the
// call sites cost nothing at all rather than costing a branch.
#define ATLAS_LOG_TRACE(...) \
    do {                     \
    } while (false)
#define ATLAS_LOG_DEBUG(...) \
    do {                     \
    } while (false)
#else
#define ATLAS_LOG_TRACE(...)                                                         \
    do {                                                                             \
        if (::atlas::LogEnabled(::atlas::LogLevel::Trace)) {                         \
            ::atlas::LogWrite(::atlas::LogLevel::Trace, ::std::format(__VA_ARGS__)); \
        }                                                                            \
    } while (false)
#define ATLAS_LOG_DEBUG(...)                                                         \
    do {                                                                             \
        if (::atlas::LogEnabled(::atlas::LogLevel::Debug)) {                         \
            ::atlas::LogWrite(::atlas::LogLevel::Debug, ::std::format(__VA_ARGS__)); \
        }                                                                            \
    } while (false)
#endif

#define ATLAS_LOG_INFO(...)                                                         \
    do {                                                                            \
        if (::atlas::LogEnabled(::atlas::LogLevel::Info)) {                         \
            ::atlas::LogWrite(::atlas::LogLevel::Info, ::std::format(__VA_ARGS__)); \
        }                                                                           \
    } while (false)

#define ATLAS_LOG_WARN(...)                                                         \
    do {                                                                            \
        if (::atlas::LogEnabled(::atlas::LogLevel::Warn)) {                         \
            ::atlas::LogWrite(::atlas::LogLevel::Warn, ::std::format(__VA_ARGS__)); \
        }                                                                           \
    } while (false)

#define ATLAS_LOG_ERROR(...)                                                         \
    do {                                                                             \
        if (::atlas::LogEnabled(::atlas::LogLevel::Error)) {                         \
            ::atlas::LogWrite(::atlas::LogLevel::Error, ::std::format(__VA_ARGS__)); \
        }                                                                            \
    } while (false)

#define ATLAS_LOG_FATAL(...)                                                         \
    do {                                                                             \
        if (::atlas::LogEnabled(::atlas::LogLevel::Fatal)) {                         \
            ::atlas::LogWrite(::atlas::LogLevel::Fatal, ::std::format(__VA_ARGS__)); \
        }                                                                            \
    } while (false)
