// =============================================================================
// spdlog 비동기 로거 구성과 LogWrite 구현. 로깅 실패는 호출자에게 번지지 않음
// =============================================================================

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

namespace atlas
{
namespace
{

constexpr std::size_t kLogLevelCount = 7;  // Trace .. Off

// [AD 11.3] 손으로 만든 락프리 큐 없음. 파일 싱크가 유일한 소비자
constexpr std::size_t kLogQueueSize = 8192;
constexpr std::size_t kLogDrainThreads = 1;

std::array< std::atomic< UInt64 >, kLogLevelCount > g_log_counts{};

// 소유자가 로거를 살려 두고 핫 패스는 atomic 만 읽음. 종료가 경합 없이 내림
std::shared_ptr< spdlog::logger > g_logger_owner;
std::atomic< spdlog::logger* > g_logger{ nullptr };

spdlog::level::level_enum ToSpdLevel( LogLevel level ) noexcept
{
    switch ( level )
    {
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

void LogInit( const LogConfig& config )
{
    LogShutdown();

    std::vector< spdlog::sink_ptr > sinks;
    if ( config.console )
    {
        sinks.push_back( std::make_shared< spdlog::sinks::stdout_color_sink_mt >() );
    }
    if ( !config.directory.empty() )
    {
        std::filesystem::create_directories( config.directory );
        const std::filesystem::path base =
            std::filesystem::path( config.directory ) / ( config.basename + ".log" );
        // 자정 롤링, retain_days 개 보존. 이름은 <base>_YYYY-MM-DD
        sinks.push_back( std::make_shared< spdlog::sinks::daily_file_sink_mt >(
            base.string(), 0, 0, false, config.retain_days ) );
    }

    // 파일 쓰기가 I/O 스레드를 막으면 안 됨. 호출자는 큐에 넣기만 함
    spdlog::init_thread_pool( kLogQueueSize, kLogDrainThreads );
    auto logger = std::make_shared< spdlog::async_logger >( "atlas", sinks.begin(), sinks.end(),
                                                            spdlog::thread_pool(),
                                                            spdlog::async_overflow_policy::block );
    // 심각도 게이트는 LogEnabled 가 이미 통과시킴. spdlog 가 두 번 거르면 안 됨
    logger->set_level( spdlog::level::trace );
    logger->flush_on( ToSpdLevel( config.flush_level ) );
    logger->set_pattern( "[%Y-%m-%d %H:%M:%S.%e] [%L] [%t] [%s:%#] %v" );

    g_logger_owner = logger;
    g_logger.store( logger.get(), std::memory_order_release );

    SetLogLevel( config.level );
    if ( !config.directory.empty() )
    {
        CrashDiagnosticsConfig crash_config;
        crash_config.directory = ( std::filesystem::path( config.directory ) / "crash" ).string();
        crash_config.basename = config.basename;
        CrashDiagnosticsInit( crash_config );
    }
}

void LogShutdown() noexcept
{
    CrashDiagnosticsShutdown();
    g_logger.store( nullptr, std::memory_order_release );
    if ( g_logger_owner )
    {
        try
        {
            g_logger_owner->flush();
        }
        catch ( ... )  // NOLINT - 종료 경로는 throw 하면 안 됨
        {
        }
        g_logger_owner.reset();
    }
    spdlog::shutdown();
}

void LogFlush() noexcept
{
    spdlog::logger* logger = g_logger.load( std::memory_order_acquire );
    if ( logger == nullptr )
    {
        return;
    }
    try
    {
        logger->flush();
    }
    catch ( ... )  // NOLINT - LogShutdown 참고
    {
    }
}

void SetLogLevel( LogLevel level ) noexcept
{
    detail::g_log_level.store( static_cast< UInt8 >( level ), std::memory_order_relaxed );
}

LogLevel CurrentLogLevel() noexcept
{
    return static_cast< LogLevel >( detail::g_log_level.load( std::memory_order_relaxed ) );
}

UInt64 LogCount( LogLevel level ) noexcept
{
    const auto index = static_cast< std::size_t >( level );
    if ( index >= kLogLevelCount )
    {
        return 0;
    }
    return g_log_counts[index].load( std::memory_order_relaxed );
}

void LogWrite( LogLevel level, std::string_view message, std::source_location loc ) noexcept
{
    const auto index = static_cast< std::size_t >( level );
    if ( index < kLogLevelCount )
    {
        g_log_counts[index].fetch_add( 1, std::memory_order_relaxed );
    }

    spdlog::logger* logger = g_logger.load( std::memory_order_acquire );
    if ( logger == nullptr )
    {
        return;
    }

    try
    {
        // [AD 11.1] ctx 원장을 호출부가 아니라 여기서 주입
        const Ctx& ctx = CurrentCtx();
        const std::string line =
            std::format( "[trace={} sid={} cid={}] {}", ctx.trace_id, IdValue( ctx.session_id ),
                         IdValue( ctx.character_id ), message );
        // spdlog::source_loc 은 줄 번호가 plain int. 서드파티 경계라 int 금지 밖
        const spdlog::source_loc where{ loc.file_name(), static_cast< int >( loc.line() ),
                                        loc.function_name() };
        logger->log( where, ToSpdLevel( level ),
                     spdlog::string_view_t{ line.data(), line.size() } );
    }
    catch ( ... )  // NOLINT - 로거 실패가 호출자를 끌고 내려가면 안 됨
    {
    }
}

}  // namespace atlas
