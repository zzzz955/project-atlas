// =============================================================================
// [AD 10.1] ORM 런타임 통합 스위트. 실제 MySQL 에 붙어 돌아감
// docker compose --env-file server/.env up -d mysql
// =============================================================================

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "atlas/config/secret_config.h"
#include "atlas/core/ctx.h"
#include "atlas/core/error.h"
#include "atlas/core/ids.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/db/connection_pool.h"
#include "atlas/db/db_runner.h"
#include "atlas/db/prepared_statement.h"
#include "atlas/db/transaction.h"
#include "atlas/net/io_runner.h"
#include "atlas/net/net_types.h"
#include "generated/db/characters_row.h"
#include "tests/optional_assert.h"

// 가짜가 가질 수 없는 것만 대상으로 삼는다
// prepared statement 재사용, 트랜잭션 가시성, 호출 스레드를 막는 드라이버
// DB 가 안 닿으면 SKIP 하고 그렇게 말한다. 못 증명한 초록은 빨강보다 나쁘다
// [AD 15.4] core-purity 가 atlas/** -> generated/** 를 금지한다
// 그래서 런타임은 범용으로 두고 행 매핑은 코어 위에 둔다

namespace
{

using atlas::CharacterId;
using atlas::Ctx;
using atlas::DbConnectionConfig;
using atlas::DbRow;
using atlas::DbValue;
using atlas::Int64;
using atlas::SysTime;
using atlas::TxState;
using atlas::UInt16;
using atlas::UInt64;

// 다른 스위트도 시드 데이터도 쓰지 않는 server_id. 이 파일이 쓴 행만 남음
constexpr UInt64 kTestServerId = 30001;

// 이 계층의 다른 것들처럼 고정 텍스트 + 플레이스홀더. 문자열 연결 없음
constexpr std::string_view kDeleteAllForServerSql =
    "DELETE FROM `characters` WHERE `server_id` = ?";

std::optional< DbConnectionConfig > LoadConfig()
{
    const atlas::SecretConfig secrets = atlas::SecretConfig::FromEnvironment();
    if ( secrets.db_host.empty() || secrets.db_name.empty() || secrets.db_user.empty() )
    {
        return std::nullopt;
    }
    DbConnectionConfig config;
    config.host = secrets.db_host;
    config.port = secrets.db_port == 0 ? UInt16{ 3306 } : secrets.db_port;
    config.database = secrets.db_name;
    config.user = secrets.db_user;
    config.password = secrets.db_password;
    // game_equip_test.cpp 참고. 스위트도 서버와 같은 TLS 조건으로 접속
    config.tls_no_verify = secrets.db_tls_no_verify;
#if defined( ATLAS_MARIADB_PLUGIN_DIR )
    // configure 시점에 박음. 작업 디렉터리 유도면 ctest 밖에선 전부 skip
    config.plugin_directory = ATLAS_MARIADB_PLUGIN_DIR;
#endif
    return config;
}

// 초 단위 절삭. schema.sql 의 DATETIME 에는 소수부가 없다
// 마이크로초가 든 값은 왕복을 넘기지 못한다
// 그러면 비교가 계층이 아니라 스키마를 시험하게 된다
SysTime NowToSecond()
{
    return std::chrono::floor< std::chrono::seconds >( atlas::SysClock::now() );
}

std::vector< DbValue > MakeCharacterRow( UInt64 character_id, const std::string& name,
                                         SysTime created_at )
{
    // 바인딩 순서는 kCharactersInsertBinding. 생성된 선언 순서
    return { DbValue{ kTestServerId },  DbValue{ character_id },
             DbValue{ UInt64{ 9001 } }, DbValue{ name },
             DbValue{ Int64{ 12 } },    DbValue{ Int64{ -34 } },
             DbValue{ UInt64{ 7 } },    DbValue{ UInt64{ 4242 } },
             DbValue{ created_at },     DbValue{ std::monostate{} } };
}

// 연결이 실제로 죽는 방식은 서버 쪽이 닫는 것(재시작, 유휴 wait_timeout 만료)
// 다른 연결의 KILL 이 공유 컨테이너를 세우지 않고 그 상황을 그대로 재현
constexpr std::string_view kConnectionIdSql = "SELECT CONNECTION_ID()";
constexpr std::string_view kKillConnectionSql = "KILL ?";

UInt64 DriverConnectionId( atlas::Connection& connection )
{
    const std::vector< DbValue > no_parameters;
    const std::vector< DbRow > rows = connection.Prepare( kConnectionIdSql ).Query( no_parameters );
    return rows.empty() ? UInt64{ 0 } : std::get< UInt64 >( rows[0][0] );
}

void KillDriverConnection( atlas::Connection& killer, UInt64 connection_id )
{
    const std::vector< DbValue > parameters{ DbValue{ connection_id } };
    killer.Prepare( kKillConnectionSql ).Execute( parameters );
}

class DbRuntimeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::optional< DbConnectionConfig > loaded = LoadConfig();
        if ( !loaded.has_value() )
        {
            GTEST_SKIP() << "SKIPPED: no database configured. Set ATLAS_DB_HOST / ATLAS_DB_NAME / "
                            "ATLAS_DB_USER (and ATLAS_DB_PASSWORD) and start MySQL with "
                            "`docker compose --env-file server/.env up -d mysql`. Running from the "
                            "host, ATLAS_DB_HOST must be 127.0.0.1 rather than the compose service "
                            "name.";
        }
        config_ = *loaded;

        try
        {
            probe_ = std::make_unique< atlas::Connection >( config_ );
        }
        catch ( const std::exception& ex )
        {
            GTEST_SKIP() << "SKIPPED: ATLAS_DB_HOST is set but the database refused the "
                            "connection: "
                         << ex.what();
        }
        Cleanup( *probe_ );
    }

    void TearDown() override
    {
        if ( probe_ )
        {
            Cleanup( *probe_ );
        }
    }

    static void Cleanup( atlas::Connection& connection )
    {
        const std::array< DbValue, 1 > parameters{ DbValue{ kTestServerId } };
        connection.Prepare( kDeleteAllForServerSql ).Execute( parameters );
    }

    static std::vector< DbRow > SelectByPk( atlas::Connection& connection, UInt64 character_id )
    {
        const std::array< DbValue, 2 > parameters{ DbValue{ kTestServerId },
                                                   DbValue{ character_id } };
        return connection.Prepare( atlas::generated::kCharactersSelectByPkSql ).Query( parameters );
    }

    DbConnectionConfig config_{};
    std::unique_ptr< atlas::Connection > probe_;
};

TEST_F( DbRuntimeTest, PoolLeasesUpToItsSizeAndThenMakesTheNextCallerWait )
{
    atlas::ConnectionPool pool( config_, 2 );
    ASSERT_EQ( pool.Size(), 2U );
    ASSERT_EQ( pool.Available(), 2U );

    std::optional< atlas::PooledConnection > first = pool.Acquire( atlas::Seconds{ 2 } );
    std::optional< atlas::PooledConnection > second = pool.Acquire( atlas::Seconds{ 2 } );
    ASSERT_TRUE( first.has_value() );
    ASSERT_TRUE( second.has_value() );
    EXPECT_EQ( pool.Available(), 0U );

    EXPECT_FALSE( pool.Acquire( atlas::Millis{ 100 } ).has_value() );

    // 리스를 돌려줘야 풀리고, 그 반납을 하는 것이 RAII 범위
    first.reset();
    EXPECT_EQ( pool.Available(), 1U );
    std::optional< atlas::PooledConnection > third = pool.Acquire( atlas::Seconds{ 2 } );
    EXPECT_TRUE( third.has_value() );
}

TEST_F( DbRuntimeTest, ExhaustedPoolFailsAfterTheTimeoutInsteadOfWaitingForever )
{
    atlas::ConnectionPool pool( config_, 1 );
    const std::optional< atlas::PooledConnection > held = pool.Acquire( atlas::Seconds{ 2 } );
    ASSERT_TRUE( held.has_value() );

    const atlas::TimePoint started = atlas::Clock::now();
    const std::optional< atlas::PooledConnection > denied = pool.Acquire( atlas::Millis{ 250 } );
    const atlas::Duration elapsed = atlas::Clock::now() - started;

    EXPECT_FALSE( denied.has_value() );
    EXPECT_GE( elapsed, atlas::Millis{ 200 } );
    EXPECT_LT( elapsed, atlas::Seconds{ 5 } );
}

TEST_F( DbRuntimeTest, ACachedStatementIsNotPreparedTwice )
{
    atlas::Connection connection( config_ );
    EXPECT_EQ( connection.PrepareCount(), 0U );

    atlas::PreparedStatement& first =
        connection.Prepare( atlas::generated::kCharactersSelectByPkSql );
    EXPECT_EQ( connection.PrepareCount(), 1U );

    atlas::PreparedStatement& second =
        connection.Prepare( atlas::generated::kCharactersSelectByPkSql );
    // 같은 객체이고 카운터도 그대로. 재준비 0회
    EXPECT_EQ( &first, &second );
    EXPECT_EQ( connection.PrepareCount(), 1U );

    connection.Prepare( atlas::generated::kCharactersInsertSql );
    EXPECT_EQ( connection.PrepareCount(), 2U );

    // 생성된 statement 텍스트와 바인딩 순서는 컴파일 타임에 합의. 서버도 같은지 확인
    EXPECT_EQ( first.ParameterCount(), atlas::generated::kCharactersSelectByPkBinding.size() );
}

TEST_F( DbRuntimeTest, CommittedTransactionIsVisibleAfterwards )
{
    atlas::Connection writer( config_ );
    Ctx ctx;
    ctx.trace_id = 4711;

    const SysTime created_at = NowToSecond();
    const std::vector< DbValue > row = MakeCharacterRow( 101, "commit-case", created_at );
    {
        atlas::Transaction transaction( writer, ctx );
        EXPECT_EQ( ctx.tx_state, TxState::Active );
        EXPECT_EQ( writer.Prepare( atlas::generated::kCharactersInsertSql ).Execute( row ), 1U );
        transaction.Commit();
    }
    EXPECT_EQ( ctx.tx_state, TxState::Committed );

    // 다른 연결에서 읽음. 쓰는 세션만 볼 수 있는 값은 커밋된 것이 아님
    atlas::Connection reader( config_ );
    const std::vector< DbRow > found = SelectByPk( reader, 101 );
    ASSERT_EQ( found.size(), 1U );
    ASSERT_EQ( found[0].size(), atlas::generated::kCharactersColumnCount );

    EXPECT_EQ( std::get< UInt64 >( found[0][atlas::generated::kCharactersColServerId] ),
               kTestServerId );
    EXPECT_EQ( std::get< std::string >( found[0][atlas::generated::kCharactersColName] ),
               "commit-case" );
    // 음수는 unsigned 플래그를 가정이 아니라 필드에서 읽고 있다는 증거
    EXPECT_EQ( std::get< Int64 >( found[0][atlas::generated::kCharactersColPosY] ), -34 );
    EXPECT_EQ( std::get< SysTime >( found[0][atlas::generated::kCharactersColCreatedAt] ),
               created_at );
    // NULL 컬럼은 0 값이 아니라 monostate
    EXPECT_TRUE( std::holds_alternative< std::monostate >(
        found[0][atlas::generated::kCharactersColLastLoginAt] ) );
}

TEST_F( DbRuntimeTest, TransactionRollsBackWhenTheScopeUnwindsInsideGuarded )
{
    atlas::Connection writer( config_ );
    Ctx ctx;
    ctx.trace_id = 4712;
    ctx.character_id = CharacterId{ 102 };

    const std::vector< DbValue > row = MakeCharacterRow( 102, "rollback-case", NowToSecond() );

    // [AD 10] 맨 try/catch 가 아니라 진짜 가드로 돌린다
    // RAII 범위와 예외 가드는 한 세트
    // 가드가 throw 를 삼켜도 원장엔 열림 -> 롤백이 남아야 한다
    atlas::Guarded(
        ctx,
        [&]
        {
            atlas::Transaction transaction( writer, ctx );
            writer.Prepare( atlas::generated::kCharactersInsertSql ).Execute( row );
            throw std::runtime_error( "deliberate failure inside the transaction scope" );
        } )();

    // 이 단언이 가능한 것은 Transaction 이 호출자의 Ctx 에 직접 쓰기 때문
    EXPECT_EQ( ctx.tx_state, TxState::RolledBack );

    atlas::Connection reader( config_ );
    EXPECT_TRUE( SelectByPk( reader, 102 ).empty() );
}

TEST_F( DbRuntimeTest, PoolRevivesAConnectionTheServerClosedUnderneathIt )
{
    atlas::ConnectionPool pool( config_, 1 );

    UInt64 killed_id = 0;
    {
        std::optional< atlas::PooledConnection > lease = pool.Acquire( atlas::Seconds{ 2 } );
        ATLAS_ASSERT_HAS_VALUE( lease );
        killed_id = DriverConnectionId( **lease );
        KillDriverConnection( *probe_, killed_id );
    }  // 죽은 연결이 운영에서와 똑같이 풀로 돌아감

    // 이 사이에 프로세스 재시작 없음. 예전엔 풀이 시체를 다시 내줬음
    std::optional< atlas::PooledConnection > revived = pool.Acquire( atlas::Seconds{ 5 } );
    ATLAS_ASSERT_HAS_VALUE( revived );
    // 가드 직후 한 번만 묶음. EXPECT_ 본문 안에서 다시 deref 하지 않음
    atlas::PooledConnection& lease = *revived;

    // 서버 쪽 스레드 id 가 달라야 새 소켓. 실제 statement 를 처리해야 쓸 수 있는 핸들
    EXPECT_NE( DriverConnectionId( *lease ), killed_id );
    const std::vector< DbValue > parameters{ DbValue{ kTestServerId } };
    EXPECT_NO_THROW( lease->Prepare( kDeleteAllForServerSql ).Execute( parameters ) );
}

TEST_F( DbRuntimeTest, ReconnectDropsThePreparedStatementCache )
{
    atlas::Connection connection( config_ );
    const UInt64 killed_id = DriverConnectionId( connection );
    connection.Prepare( atlas::generated::kCharactersSelectByPkSql );
    const UInt64 prepared_before = connection.PrepareCount();
    ASSERT_EQ( prepared_before, 2U );

    KillDriverConnection( *probe_, killed_id );
    ASSERT_TRUE( connection.EnsureAlive() );
    EXPECT_NE( DriverConnectionId( connection ), killed_id );

    // 증거는 포인터 비교가 아니라 카운터
    // 옛 객체는 캐시와 함께 파괴되어 주소 비교는 해제된 메모리 읽기
    // 같은 sql 이 다시 준비되지 않으면 캐시는 죽은 MYSQL_STMT 를 쥔 채로 남는다
    // 그 상태의 드라이버 접근은 use-after-free
    connection.Prepare( atlas::generated::kCharactersSelectByPkSql );
    EXPECT_EQ( connection.PrepareCount(), prepared_before + 2U );
}

TEST_F( DbRuntimeTest, ReconnectCannotInventADatabaseThatIsNotThere )
{
    // 재접속과 같은 진입점(Connection::Open)을 아무것도 없는 엔드포인트로 겨눔
    // 실패하면 EnsureAlive 는 false, 풀은 기존 unavailable 응답. 조용한 성공 없음
    // [AD 10] 종단 버전(docker compose restart mysql)은 공유 컨테이너라 손으로 검증
    DbConnectionConfig unreachable = config_;
    unreachable.port = UInt16{ 1 };

    EXPECT_THROW( static_cast< void >( std::make_unique< atlas::Connection >( unreachable ) ),
                  atlas::DbException );
    EXPECT_THROW(
        static_cast< void >( std::make_unique< atlas::ConnectionPool >( unreachable, 1 ) ),
        atlas::DbException );
}

TEST_F( DbRuntimeTest, CtxCrossesIntoTheDbThreadByValue )
{
    atlas::ConnectionPool pool( config_, 2 );
    atlas::DbRunner runner( pool, 2, atlas::Seconds{ 2 } );
    runner.Start();

    Ctx ctx;
    ctx.trace_id = 987654321;
    ctx.character_id = CharacterId{ 55 };

    std::atomic< UInt64 > observed_trace{ 0 };
    std::atomic< UInt64 > observed_character{ 0 };
    std::atomic< bool > had_connection{ false };
    std::atomic< bool > done{ false };

    const atlas::SubmitResult submitted = runner.Submit(
        ctx,
        [&]( Ctx& job_ctx, atlas::Connection* connection )
        {
            // job_ctx 가 아니라 설치된 원장에서 읽는다
            // 이 스레드의 로그 매크로가 읽는 그것이 건너왔는지가 관심사
            observed_trace.store( atlas::CurrentCtx().trace_id );
            observed_character.store( atlas::IdValue( atlas::CurrentCtx().character_id ) );
            had_connection.store( connection != nullptr );
            job_ctx.tx_state = TxState::Committed;
        },
        [&]( const Ctx& done_ctx )
        {
            // 잡이 남긴 원장이 완료 콜백까지 실려 옴
            EXPECT_EQ( done_ctx.tx_state, TxState::Committed );
            done.store( true );
        } );
    ASSERT_EQ( submitted, atlas::SubmitResult::Accepted );

    const atlas::TimePoint deadline = atlas::Clock::now() + atlas::Seconds{ 5 };
    while ( !done.load() && atlas::Clock::now() < deadline )
    {
        std::this_thread::yield();
    }
    runner.Stop();

    EXPECT_TRUE( done.load() );
    EXPECT_TRUE( had_connection.load() );
    EXPECT_EQ( observed_trace.load(), 987654321U );
    EXPECT_EQ( observed_character.load(), 55U );
    // 호출자의 원장은 그대로. 잡은 사본을 바꿈
    EXPECT_EQ( ctx.tx_state, TxState::None );
}

TEST_F( DbRuntimeTest, CompletionRunsBackOnTheOriginatingStrand )
{
    atlas::IoRunner io_runner( 1 );
    io_runner.Start();
    atlas::Strand strand = atlas::asio::make_strand( io_runner.Context() );

    atlas::ConnectionPool pool( config_, 1 );
    atlas::DbRunner runner( pool, 1, atlas::Seconds{ 2 } );
    runner.Start();

    std::atomic< bool > job_off_strand{ false };
    std::atomic< bool > completion_on_strand{ false };
    std::atomic< bool > done{ false };

    Ctx ctx;
    ctx.trace_id = 24680;

    const atlas::SubmitResult submitted = runner.Submit(
        ctx,
        [&]( Ctx&, atlas::Connection* connection )
        {
            // [AD 9] 블로킹 절반은 strand 위에 있으면 안 됨. 별도 풀의 존재 이유
            job_off_strand.store( !strand.running_in_this_thread() );
            ASSERT_NE( connection, nullptr );
            const std::array< DbValue, 1 > parameters{ DbValue{ kTestServerId } };
            connection->Prepare( kDeleteAllForServerSql ).Execute( parameters );
        },
        [&]( const Ctx& )
        {
            completion_on_strand.store( strand.running_in_this_thread() );
            done.store( true );
        },
        [strand]( std::function< void() > completion )
        { atlas::asio::post( strand, std::move( completion ) ); } );
    ASSERT_EQ( submitted, atlas::SubmitResult::Accepted );

    const atlas::TimePoint deadline = atlas::Clock::now() + atlas::Seconds{ 5 };
    while ( !done.load() && atlas::Clock::now() < deadline )
    {
        std::this_thread::yield();
    }
    runner.Stop();
    io_runner.Stop();

    EXPECT_TRUE( done.load() );
    EXPECT_TRUE( job_off_strand.load() );
    EXPECT_TRUE( completion_on_strand.load() );
}

// [AD 10.8] 로드 셰딩 회귀 방지선
// 상한이 없으면 큐가 무한히 자란다
// 서버는 클라이언트가 이미 포기한 요청을 계속 커밋한다
TEST_F( DbRuntimeTest, QueueRefusesPastItsCapAndStopsGrowingThere )
{
    atlas::ConnectionPool pool( config_, 1 );
    atlas::DbRunner runner( pool, 1, atlas::Seconds{ 2 } );
    const std::size_t capacity = runner.QueueCapacity();
    ASSERT_EQ( capacity, atlas::DbRunner::kMaxQueuedJobsPerThread );
    runner.Start();

    // 모든 잡이 해제될 때까지 멈춰 서서, 큐는 명령할 때만 빠짐
    std::atomic< bool > release{ false };
    std::atomic< std::size_t > ran{ 0 };
    std::atomic< std::size_t > completed{ 0 };
    std::size_t accepted = 0;
    std::size_t rejected = 0;

    for ( std::size_t index = 0; index < capacity + 8; ++index )
    {
        const atlas::SubmitResult result = runner.Submit(
            Ctx{},
            [&]( Ctx&, atlas::Connection* )
            {
                ran.fetch_add( 1 );
                while ( !release.load() )
                {
                    std::this_thread::yield();
                }
            },
            [&]( const Ctx& ) { completed.fetch_add( 1 ); } );
        if ( result == atlas::SubmitResult::Accepted )
        {
            ++accepted;
        }
        else
        {
            EXPECT_EQ( result, atlas::SubmitResult::Rejected );
            ++rejected;
        }
        // 매 submit 뒤마다 검사. 끝에서만 참인 상한은 이미 샌 상한
        EXPECT_LE( runner.PendingCount(), capacity );
    }

    // 워커 1개가 첫 잡 안에 멈춰 있어 나머지는 큐가 들고 있음
    EXPECT_GE( accepted, capacity );
    EXPECT_GT( rejected, 0U );
    EXPECT_EQ( accepted + rejected, capacity + 8 );
    EXPECT_EQ( runner.RejectedCount(), static_cast< UInt64 >( rejected ) );

    release.store( true );
    const atlas::TimePoint deadline = atlas::Clock::now() + atlas::Seconds{ 20 };
    while ( completed.load() < accepted && atlas::Clock::now() < deadline )
    {
        std::this_thread::yield();
    }
    runner.Stop();

    // 거부된 잡은 작업도 완료 콜백도 돌리지 않는다
    // 셋째 선택지인 조용한 소멸을 이 두 등식이 못 박는다
    EXPECT_EQ( ran.load(), accepted );
    EXPECT_EQ( completed.load(), accepted );
}

// 반환값 하나에 두 뜻을 얹는 것을 막는 회귀 방지선
// false 는 "멈춤" 하나였다
// 여기에 "과부하" 를 접으면 호출자가 종료와 바쁨을 구분할 수 없다
TEST_F( DbRuntimeTest, ARefusedSubmitIsDistinctFromAStoppedOne )
{
    atlas::ConnectionPool pool( config_, 1 );
    atlas::DbRunner runner( pool, 1, atlas::Seconds{ 2 } );

    // 시작한 적 없음
    EXPECT_EQ( runner.Submit( Ctx{}, []( Ctx&, atlas::Connection* ) {} ),
               atlas::SubmitResult::Stopped );
    EXPECT_EQ( runner.RejectedCount(), 0U );

    runner.Start();
    std::atomic< bool > release{ false };
    std::size_t rejected = 0;
    for ( std::size_t index = 0; index < runner.QueueCapacity() + 8; ++index )
    {
        const atlas::SubmitResult result = runner.Submit( Ctx{},
                                                          [&]( Ctx&, atlas::Connection* )
                                                          {
                                                              while ( !release.load() )
                                                              {
                                                                  std::this_thread::yield();
                                                              }
                                                          } );
        if ( result == atlas::SubmitResult::Rejected )
        {
            ++rejected;
        }
    }
    EXPECT_GT( rejected, 0U );
    EXPECT_EQ( runner.RejectedCount(), static_cast< UInt64 >( rejected ) );

    release.store( true );
    runner.Stop();

    // 드레인 뒤 정지이고 거부 카운트는 그대로. 두 답이 분리된 채로 남음
    EXPECT_EQ( runner.Submit( Ctx{}, []( Ctx&, atlas::Connection* ) {} ),
               atlas::SubmitResult::Stopped );
    EXPECT_EQ( runner.RejectedCount(), static_cast< UInt64 >( rejected ) );
}

}  // namespace
