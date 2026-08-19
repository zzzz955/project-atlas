#pragma once

// =============================================================================
// [AD 11.2] 예상 가능한 실패는 예외가 아니라 expected/error_code
// 예외는 프로그래밍 오류와 복구 불가 상태에만. 이 헤더에 asio 의존은 없음
// =============================================================================

#include <exception>
#include <format>
#include <functional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "atlas/core/ctx.h"
#include "atlas/core/log.h"
#include "atlas/core/types.h"

namespace atlas
{

// core 가 내보내는 단 하나의 예외 타입. 미리 짜 둔 계층 구조는 없음
class Exception : public std::runtime_error
{
public:
    explicit Exception( const std::string& message,
                        std::source_location where = std::source_location::current() );

    [[nodiscard]] const std::source_location& Where() const noexcept { return where_; }
    // 예외 생성 시점에 설치돼 있던 원장의 trace id. 요청과 상관 가능
    [[nodiscard]] UInt64 TraceId() const noexcept { return trace_id_; }
    [[nodiscard]] std::string_view StackTrace() const noexcept { return stack_trace_; }

private:
    std::source_location where_;
    UInt64 trace_id_;
    std::string stack_trace_;
};

namespace detail
{

void ReportGuardedException( const std::exception& failure ) noexcept;
void ReportGuardedUnknownException() noexcept;

}  // namespace detail

// [CS 6] 핸들러 소유자가 주입하는 "연결을 안전하게 내려라"
// 템플릿 인자 대신 타입 소거를 쓴다
// 가드마다 인스턴스화가 늘지 않게 하려는 것
using FailureHandler = std::function< void( std::string_view reason ) >;

// =============================================================================
// 비동기 핸들러 경계
// =============================================================================

// [AD 11.2b] 핸들러를 탈출한 예외는 io_context::run() 까지 올라간다
// I/O 스레드가 죽고 그 스레드가 맡던 세션 전부가 조용히 멎는다
// 모든 진입점을 이걸로 감싼다
// [CS 5] 매크로 아닌 템플릿
// 매크로는 중첩/return 을 틀리고 디버거가 들어가지 못한다
template < class Handler >
class Guarded
{
public:
    Guarded( Ctx ctx, Handler handler ) : ctx_( ctx ), handler_( std::move( handler ) ) {}

    Guarded( Ctx ctx, Handler handler, FailureHandler on_failure )
        : ctx_( ctx ), handler_( std::move( handler ) ), on_failure_( std::move( on_failure ) )
    {
    }

    template < class... Args >
    void operator()( Args&&... args ) noexcept
    {
        const CtxScope scope( ctx_ );
        try
        {
            handler_( std::forward< Args >( args )... );
        }
        catch ( const std::exception& ex )
        {
            Fail( ex );
        }
        catch ( ... )
        {
            FailUnknown();
        }
    }

private:
    void Fail( const std::exception& failure ) noexcept
    {
        // 마지막 방어선이라 보고 경로도 전부여야 함
        // 로그의 할당 실패가 terminate 로 번지면 안 됨
        try
        {
            detail::ReportGuardedException( failure );
            if ( on_failure_ )
            {
                on_failure_( failure.what() );
            }
        }
        catch ( ... )
        {  // NOLINT - 더 보고할 곳이 없음
        }
    }

    void FailUnknown() noexcept
    {
        try
        {
            detail::ReportGuardedUnknownException();
            if ( on_failure_ )
            {
                on_failure_( "non-std exception" );
            }
        }
        catch ( ... )
        {  // NOLINT - 더 보고할 곳이 없음
        }
    }

    Ctx ctx_;
    Handler handler_;
    FailureHandler on_failure_;
};

template < class Handler >
Guarded( Ctx, Handler ) -> Guarded< Handler >;

template < class Handler >
Guarded( Ctx, Handler, FailureHandler ) -> Guarded< Handler >;

}  // namespace atlas

// [CS 5] 화이트리스트는 로그 매크로 6개 + 여기 3개로 닫힘

// std::format 메시지에 소스 위치와 ctx 를 붙여 Type 을 throw
#define ATLAS_THROW( Type, ... ) throw Type( ::std::format( __VA_ARGS__ ) )

// 릴리스에서도 살아 있음. 위반은 프로세스가 이미 추론 불가 상태라는 뜻
#define ATLAS_CHECK( cond, ... )                            \
    do                                                      \
    {                                                       \
        if ( !( cond ) )                                    \
        {                                                   \
            ATLAS_THROW( ::atlas::Exception, __VA_ARGS__ ); \
        }                                                   \
    } while ( false )

#if defined( NDEBUG )
// 릴리스에서 사라져 조건이 평가되지 않음. 부작용 금지. 살아남아야 하면 CHECK
#define ATLAS_ASSERT( cond ) \
    do                       \
    {                        \
    } while ( false )
#else
#define ATLAS_ASSERT( cond ) ATLAS_CHECK( cond, "assertion failed: {}", #cond )
#endif
