#include "atlas/core/log.h"

#include <spdlog/async.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

#include "atlas/core/crash.h"
#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"

namespace atlas {
namespace {

constexpr std::size_t kLogLevelCount = 7;  // Trace .. Off

// architecture-design.md §11.3 — no hand-written lock-free queue. spdlog's own async queue is the
// answer for the log path, and one drain thread is enough: the file sink is the only consumer.
constexpr std::size_t kLogQueueSize = 8192;
constexpr std::size_t kLogDrainThreads = 1;

std::array<std::atomic<UInt64>, kLogLevelCount> g_log_counts{};

// The owner keeps the logger alive; the atomic is what the hot path reads, so LogShutdown can
// unpublish the logger without racing a concurrent LogWrite.
std::shared_ptr<spdlog::logger> g_logger_owner;
std::atomic<spdlog::logger*> g_logger{nullptr};

spdlog::level::level_enum ToSpdLevel(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return spdlog::level::trace;
        case LogLevel::Debug:
            return spdlog::level::debug;
        case LogLevel::Info:
            return spdlog::level::info;
        case LogLevel::Warn:
            return spdlog::level::warn;
        case LogLevel::Error:
            return spdlog::level::err;
        case LogLevel::Fatal:
            return spdlog::level::critical;
        case LogLevel::Off:
            return spdlog::level::off;
    }
    return spdlog::level::info;
}

}  // namespace

void LogInit(const LogConfig& config) {
    LogShutdown();

    std::vector<spdlog::sink_ptr> sinks;
    if (config.console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }
    if (!config.directory.empty()) {
        std::filesystem::create_directories(config.directory);
        const std::filesystem::path base =
            std::filesystem::path(config.directory) / (config.basename + ".log");
        // Rolls at midnight and keeps `retain_days` files; spdlog names them `<base>_YYYY-MM-DD`.
        sinks.push_back(std::make_shared<spdlog::sinks::daily_file_sink_mt>(
            base.string(), 0, 0, false, config.retain_days));
    }

    // 🔴 The file write must never block an I/O thread: the caller only formats and enqueues, and
    // a dedicated drain thread does the write.
    spdlog::init_thread_pool(kLogQueueSize, kLogDrainThreads);
    auto logger = std::make_shared<spdlog::async_logger>("atlas", sinks.begin(), sinks.end(),
                                                         spdlog::thread_pool(),
                                                         spdlog::async_overflow_policy::block);
    // The severity gate is ours (LogEnabled, checked before formatting), so spdlog must not filter
    // a second time — anything that reaches it has already passed the policy layer.
    logger->set_level(spdlog::level::trace);
    logger->flush_on(ToSpdLevel(config.flush_level));
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%L] [%t] [%s:%#] %v");

    g_logger_owner = logger;
    g_logger.store(logger.get(), std::memory_order_release);

    SetLogLevel(config.level);
    if (!config.directory.empty()) {
        CrashDiagnosticsConfig crash_config;
        crash_config.directory = (std::filesystem::path(config.directory) / "crash").string();
        crash_config.basename = config.basename;
        CrashDiagnosticsInit(crash_config);
    }
}

void LogShutdown() noexcept {
    CrashDiagnosticsShutdown();
    g_logger.store(nullptr, std::memory_order_release);
    if (g_logger_owner) {
        try {
            g_logger_owner->flush();
        } catch (...) {  // NOLINT — shutdown must not throw; there is nowhere left to report to.
        }
        g_logger_owner.reset();
    }
    spdlog::shutdown();
}

void LogFlush() noexcept {
    spdlog::logger* logger = g_logger.load(std::memory_order_acquire);
    if (logger == nullptr) {
        return;
    }
    try {
        logger->flush();
    } catch (...) {  // NOLINT — see LogShutdown.
    }
}

void SetLogLevel(LogLevel level) noexcept {
    detail::g_log_level.store(static_cast<UInt8>(level), std::memory_order_relaxed);
}

LogLevel CurrentLogLevel() noexcept {
    return static_cast<LogLevel>(detail::g_log_level.load(std::memory_order_relaxed));
}

UInt64 LogCount(LogLevel level) noexcept {
    const auto index = static_cast<std::size_t>(level);
    if (index >= kLogLevelCount) {
        return 0;
    }
    return g_log_counts[index].load(std::memory_order_relaxed);
}

void LogWrite(LogLevel level, std::string_view message, std::source_location loc) noexcept {
    const auto index = static_cast<std::size_t>(level);
    if (index < kLogLevelCount) {
        g_log_counts[index].fetch_add(1, std::memory_order_relaxed);
    }

    spdlog::logger* logger = g_logger.load(std::memory_order_acquire);
    if (logger == nullptr) {
        return;
    }

    try {
        // architecture-design.md §11.1 — the ctx ledger is injected here rather than at the call
        // site, so no caller has to remember to carry the trace id into its format string.
        const Ctx& ctx = CurrentCtx();
        const std::string line =
            std::format("[trace={} sid={} cid={}] {}", ctx.trace_id, IdValue(ctx.session_id),
                        IdValue(ctx.character_id), message);
        // spdlog::source_loc predates std::source_location and stores the line as a plain `int`;
        // the cast is a third-party boundary, not an ID/protocol/persistence field.
        const spdlog::source_loc where{loc.file_name(), static_cast<int>(loc.line()),
                                       loc.function_name()};
        logger->log(where, ToSpdLevel(level), spdlog::string_view_t{line.data(), line.size()});
    } catch (...) {  // NOLINT — a failing logger must never take the caller down with it.
    }
}

}  // namespace atlas
