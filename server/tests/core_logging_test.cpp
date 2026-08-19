// =============================================================================
// 코어 계약 검사. 로그 매크로, Guarded, Ctx 원장, 강타입 ID, 예외 스택
// =============================================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "atlas/core/ctx.h"
#include "atlas/core/error.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"
#include "atlas/core/stack_trace.h"
#include "atlas/core/types.h"

namespace
{

// [CS 5] ExpensiveDump(a) 대역. 레벨이 꺼지면 이 카운터가 0 이어야 함
atlas::UInt64 g_log_probe_calls = 0;

atlas::UInt64 LogProbe()
{
    ++g_log_probe_calls;
    return g_log_probe_calls;
}

[[noreturn]] void ThrowForeignException() { throw std::runtime_error( "foreign failure" ); }

// 로그 매크로가 존재하는 근거 그 자체. 진입점을 함수로 바꾸면 여기가 깨짐
TEST( CoreLogging, DisabledLevelDoesNotEvaluateItsArguments )
{
    atlas::SetLogLevel( atlas::LogLevel::Info );
    g_log_probe_calls = 0;
    const atlas::UInt64 debug_records_before = atlas::LogCount( atlas::LogLevel::Debug );

    ATLAS_LOG_DEBUG( "probe={}", LogProbe() );

    EXPECT_EQ( g_log_probe_calls, atlas::UInt64{ 0 } );
    EXPECT_EQ( atlas::LogCount( atlas::LogLevel::Debug ), debug_records_before );
}

TEST( CoreLogging, EnabledLevelEvaluatesItsArgumentsExactlyOnce )
{
    atlas::SetLogLevel( atlas::LogLevel::Info );
    g_log_probe_calls = 0;
    const atlas::UInt64 info_records_before = atlas::LogCount( atlas::LogLevel::Info );

    ATLAS_LOG_INFO( "probe={}", LogProbe() );

    EXPECT_EQ( g_log_probe_calls, atlas::UInt64{ 1 } );
    EXPECT_EQ( atlas::LogCount( atlas::LogLevel::Info ), info_records_before + 1 );
}

TEST( CoreLogging, LevelOffSilencesEveryChannel )
{
    atlas::SetLogLevel( atlas::LogLevel::Off );
    EXPECT_FALSE( atlas::LogEnabled( atlas::LogLevel::Fatal ) );
    EXPECT_FALSE( atlas::LogEnabled( atlas::LogLevel::Error ) );
    EXPECT_EQ( atlas::CurrentLogLevel(), atlas::LogLevel::Off );
    atlas::SetLogLevel( atlas::LogLevel::Info );
    EXPECT_TRUE( atlas::LogEnabled( atlas::LogLevel::Error ) );
    EXPECT_FALSE( atlas::LogEnabled( atlas::LogLevel::Debug ) );
}

TEST( CoreLogging, InitWritesThroughTheAsyncFileSink )
{
    // 실제 spdlog 사슬을 탐. 줄이면 LogInit 은 운영에서만 깨지는 미검증 코드
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "atlas_core_logging_test";
    std::filesystem::remove_all( dir );

    atlas::LogConfig config;
    config.directory = dir.string();
    config.basename = "atlas_test";
    config.console = false;
    config.level = atlas::LogLevel::Info;
    atlas::LogInit( config );

    ATLAS_LOG_INFO( "file sink smoke test {}", 1 );
    atlas::LogFlush();
    atlas::LogShutdown();

    ASSERT_TRUE( std::filesystem::exists( dir ) );
    EXPECT_FALSE( std::filesystem::is_empty( dir ) );

    std::filesystem::remove_all( dir );
    atlas::SetLogLevel( atlas::LogLevel::Info );
}

// [AD 11.2] 가드가 없으면 핸들러 하나가 I/O 스레드와 그 세션 전부를 멎게 함
TEST( CoreGuarded, ContainsExceptionsLogsOneErrorAndRunsTheFailureCallback )
{
    atlas::SetLogLevel( atlas::LogLevel::Info );
    const atlas::UInt64 errors_before = atlas::LogCount( atlas::LogLevel::Error );

    atlas::UInt64 closed = 0;
    atlas::Ctx ctx;
    ctx.trace_id = 42;

    auto guarded = atlas::Guarded(
        ctx, [] { ATLAS_THROW( atlas::Exception, "boom {}", 7 ); },
        atlas::FailureHandler{ [&closed]( std::string_view ) { ++closed; } } );

    static_assert( noexcept( guarded() ), "the guard must be noexcept - it is the last line" );
    EXPECT_NO_THROW( guarded() );

    EXPECT_EQ( closed, atlas::UInt64{ 1 } );
    EXPECT_EQ( atlas::LogCount( atlas::LogLevel::Error ), errors_before + 1 );
}

TEST( CoreGuarded, ContainsThrowsThatAreNotStdExceptions )
{
    atlas::SetLogLevel( atlas::LogLevel::Info );
    atlas::UInt64 closed = 0;
    atlas::Ctx ctx;

    auto guarded = atlas::Guarded(
        ctx, [] { throw 7; },
        atlas::FailureHandler{ [&closed]( std::string_view ) { ++closed; } } );
    EXPECT_NO_THROW( guarded() );
    EXPECT_EQ( closed, atlas::UInt64{ 1 } );
}

TEST( CoreGuarded, ForwardsArgumentsAndWorksWithoutAFailureCallback )
{
    atlas::UInt64 seen = 0;
    atlas::Ctx ctx;
    atlas::Guarded( ctx, [&seen]( atlas::UInt64 value ) { seen = value; } )( atlas::UInt64{ 5 } );
    EXPECT_EQ( seen, atlas::UInt64{ 5 } );

    EXPECT_NO_THROW(
        atlas::Guarded( ctx, [] { ATLAS_THROW( atlas::Exception, "no callback" ); } )() );
}

// [AD 9.2] 그냥 thread_local 이면 중첩 순간 깨짐. 설치-복원을 못 박는 테스트
TEST( CoreCtx, GuardedInstallsTheLedgerAndRestoresTheOuterOneOnExit )
{
    atlas::Ctx outer;
    outer.trace_id = 1;
    outer.session_id = atlas::SessionId{ 11 };
    outer.character_id = atlas::CharacterId{ 22 };

    const atlas::CtxScope scope( outer );
    ASSERT_EQ( atlas::CurrentCtx().trace_id, atlas::UInt64{ 1 } );

    atlas::Ctx inner;
    inner.trace_id = 2;
    inner.session_id = atlas::SessionId{ 99 };

    atlas::UInt64 seen_trace = 0;
    atlas::UInt64 seen_session = 0;
    atlas::Guarded( inner,
                    [&seen_trace, &seen_session]
                    {
                        seen_trace = atlas::CurrentCtx().trace_id;
                        seen_session = atlas::IdValue( atlas::CurrentCtx().session_id );
                    } )();

    EXPECT_EQ( seen_trace, atlas::UInt64{ 2 } );
    EXPECT_EQ( seen_session, atlas::UInt64{ 99 } );
    EXPECT_EQ( atlas::CurrentCtx().trace_id, atlas::UInt64{ 1 } );
    EXPECT_EQ( atlas::IdValue( atlas::CurrentCtx().session_id ), atlas::UInt64{ 11 } );
    EXPECT_EQ( atlas::IdValue( atlas::CurrentCtx().character_id ), atlas::UInt64{ 22 } );

    // 복원은 언와인딩 경로에서도 살아야 한다
    // 아니면 던진 핸들러 하나가 그 스레드의 이후 원장을 전부 오염시킨다
    atlas::Guarded( inner, [] { ATLAS_THROW( atlas::Exception, "boom" ); } )();
    EXPECT_EQ( atlas::CurrentCtx().trace_id, atlas::UInt64{ 1 } );
    EXPECT_EQ( atlas::IdValue( atlas::CurrentCtx().session_id ), atlas::UInt64{ 11 } );
}

TEST( CoreCtx, DefaultLedgerIsEmpty )
{
    EXPECT_EQ( atlas::CurrentCtx().trace_id, atlas::UInt64{ 0 } );
    EXPECT_EQ( atlas::CurrentCtx().tx_state, atlas::TxState::None );
}

// [CS 4.3] ID 인자 뒤바꿈을 컴파일 에러로 만들기 위한 가드
TEST( CoreIds, StrongIdsHaveNoImplicitConversions )
{
    static_assert( !std::is_convertible_v< atlas::ActorId, atlas::UInt64 > );
    static_assert( !std::is_convertible_v< atlas::UInt64, atlas::ActorId > );
    static_assert( !std::is_convertible_v< atlas::AccountId, atlas::CharacterId > );
    static_assert( !std::is_convertible_v< atlas::SessionId, atlas::ActorId > );
    static_assert( sizeof( atlas::ActorId ) == sizeof( atlas::UInt64 ) );

    EXPECT_EQ( atlas::IdValue( atlas::ActorId{ 7 } ), atlas::UInt64{ 7 } );
    EXPECT_EQ( atlas::IdValue( atlas::AccountId{ 8 } ), atlas::UInt64{ 8 } );
}

TEST( CoreError, ThrowAndCheckCarrySourceLocationAndTrace )
{
    atlas::Ctx ctx;
    ctx.trace_id = 1234;
    const atlas::CtxScope scope( ctx );

    // 일부러 비const. 리터럴로 초기화한 const 정수는 여전히 상수식
    // 그러면 /W4 가 ATLAS_CHECK 안의 조건에 C4127 을 낸다
    atlas::Int32 expected = 1;
    atlas::Int32 actual = 2;

    try
    {
        ATLAS_CHECK( expected == actual, "mismatch {} vs {}", expected, actual );
        FAIL() << "ATLAS_CHECK did not throw";
    }
    catch ( const atlas::Exception& ex )
    {
        EXPECT_EQ( ex.TraceId(), atlas::UInt64{ 1234 } );
        EXPECT_NE( std::string_view{ ex.what() }.find( "mismatch 1 vs 2" ),
                   std::string_view::npos );
        EXPECT_NE( std::string_view{ ex.Where().file_name() }, std::string_view{} );
        ASSERT_FALSE( ex.StackTrace().empty() );
        EXPECT_NE( ex.StackTrace().find( "0x" ), std::string_view::npos ) << ex.StackTrace();
#if defined( _WIN32 )
        EXPECT_NE( ex.StackTrace().find( "core_logging_test.cpp" ), std::string_view::npos )
            << ex.StackTrace();
#endif
    }

    EXPECT_NO_THROW( ATLAS_CHECK( expected < actual, "never" ) );
    EXPECT_NO_THROW( ATLAS_ASSERT( expected < actual ) );
}

TEST( CoreError, ForeignExceptionKeepsItsThrowSiteStack )
{
    try
    {
        ThrowForeignException();
    }
    catch ( const std::runtime_error& )
    {
        const std::string trace = atlas::CaptureCurrentExceptionStackTrace();
        ASSERT_FALSE( trace.empty() );
        EXPECT_NE( trace.find( "0x" ), std::string::npos ) << trace;
#if defined( _WIN32 )
        EXPECT_NE( trace.find( "core_logging_test.cpp" ), std::string::npos ) << trace;
        EXPECT_NE( trace.find( "ThrowForeignException" ), std::string::npos ) << trace;
#endif
    }
}

}  // namespace
