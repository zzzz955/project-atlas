#pragma once

// =============================================================================
// [AD 11.1] 로깅 파사드
// 모든 TU 가 포함하는 헤더라 무거운 spdlog 는 log.cpp 뒤에 숨긴다
// 호출부는 std::format 으로 문자열을 만든다
// =============================================================================

#include <atomic>
#include <format>
#include <source_location>
#include <string>
#include <string_view>

#include "atlas/core/types.h"

namespace atlas
{

// 심각도 순. 레벨 게이트가 정수 비교 한 번으로 끝남
enum class LogLevel : UInt8
{
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Off
};

struct LogConfig
{
    // 비어 있으면 파일 싱크 없음 (테스트, 콘솔만 원하는 경우)
    std::string directory{};
    std::string basename{ "atlas" };
    LogLevel level{ LogLevel::Info };
    // 이 레벨 이상은 드레인을 기다리지 않고 즉시 flush
    LogLevel flush_level{ LogLevel::Warn };
    // [AD 11.1] 일 단위 롤링 보존. 파일 수 = 일수
    UInt16 retain_days{ 14 };
    bool console{ true };
};

// 동시 로깅에 안전하지 않음. 프로세스 수명을 쥔 스레드에서만 호출할 것
void LogInit( const LogConfig& config );
void LogShutdown() noexcept;
void LogFlush() noexcept;

void SetLogLevel( LogLevel level ) noexcept;
LogLevel CurrentLogLevel() noexcept;

// "가드가 ERROR 를 정확히 하나 남겼다" 를 테스트 가능한 문장으로 만듦
UInt64 LogCount( LogLevel level ) noexcept;

namespace detail
{

// 함수 지역 static 이면 모든 로그 호출부 앞에서 스레드 안전 초기화 검사를 물음
inline std::atomic< UInt8 > g_log_level{ static_cast< UInt8 >( LogLevel::Info ) };

}  // namespace detail

// 레벨 검사가 포매팅보다 먼저. 꺼진 레벨은 문자열을 하나도 만들지 않음
[[nodiscard]] inline bool LogEnabled( LogLevel level ) noexcept
{
    return static_cast< UInt8 >( level ) >= detail::g_log_level.load( std::memory_order_relaxed );
}

// [CS 5] loc 은 기본 인자. 매크로 확장 지점에서 평가돼 __FILE__ 이 불필요
void LogWrite( LogLevel level, std::string_view message,
               std::source_location loc = std::source_location::current() ) noexcept;

}  // namespace atlas

// [CS 5] 매크로인 유일한 근거는 지연 평가
// 함수는 인자를 먼저 평가한다
// 꺼진 DEBUG 에서도 ExpensiveDump() 를 호출해 버린다
// 공용 IMPL 헬퍼는 없다

#if defined( NDEBUG )
// 매크로만 할 수 있는 둘째. 릴리스 바이너리에서 TRACE/DEBUG 를 통째로 삭제
#define ATLAS_LOG_TRACE( ... ) \
    do                         \
    {                          \
    } while ( false )
#define ATLAS_LOG_DEBUG( ... ) \
    do                         \
    {                          \
    } while ( false )
#else
#define ATLAS_LOG_TRACE( ... )                                                           \
    do                                                                                   \
    {                                                                                    \
        if ( ::atlas::LogEnabled( ::atlas::LogLevel::Trace ) )                           \
        {                                                                                \
            ::atlas::LogWrite( ::atlas::LogLevel::Trace, ::std::format( __VA_ARGS__ ) ); \
        }                                                                                \
    } while ( false )
#define ATLAS_LOG_DEBUG( ... )                                                           \
    do                                                                                   \
    {                                                                                    \
        if ( ::atlas::LogEnabled( ::atlas::LogLevel::Debug ) )                           \
        {                                                                                \
            ::atlas::LogWrite( ::atlas::LogLevel::Debug, ::std::format( __VA_ARGS__ ) ); \
        }                                                                                \
    } while ( false )
#endif

#define ATLAS_LOG_INFO( ... )                                                           \
    do                                                                                  \
    {                                                                                   \
        if ( ::atlas::LogEnabled( ::atlas::LogLevel::Info ) )                           \
        {                                                                               \
            ::atlas::LogWrite( ::atlas::LogLevel::Info, ::std::format( __VA_ARGS__ ) ); \
        }                                                                               \
    } while ( false )

#define ATLAS_LOG_WARN( ... )                                                           \
    do                                                                                  \
    {                                                                                   \
        if ( ::atlas::LogEnabled( ::atlas::LogLevel::Warn ) )                           \
        {                                                                               \
            ::atlas::LogWrite( ::atlas::LogLevel::Warn, ::std::format( __VA_ARGS__ ) ); \
        }                                                                               \
    } while ( false )

#define ATLAS_LOG_ERROR( ... )                                                           \
    do                                                                                   \
    {                                                                                    \
        if ( ::atlas::LogEnabled( ::atlas::LogLevel::Error ) )                           \
        {                                                                                \
            ::atlas::LogWrite( ::atlas::LogLevel::Error, ::std::format( __VA_ARGS__ ) ); \
        }                                                                                \
    } while ( false )

#define ATLAS_LOG_FATAL( ... )                                                           \
    do                                                                                   \
    {                                                                                    \
        if ( ::atlas::LogEnabled( ::atlas::LogLevel::Fatal ) )                           \
        {                                                                                \
            ::atlas::LogWrite( ::atlas::LogLevel::Fatal, ::std::format( __VA_ARGS__ ) ); \
        }                                                                                \
    } while ( false )
