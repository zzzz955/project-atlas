// =============================================================================
// [AD 10.4] 서버 경계를 넘는 원자성의 결함 주입 스위트
// =============================================================================

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <format>
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
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/db/connection_pool.h"
#include "atlas/db/idempotency.h"
#include "atlas/db/prepared_statement.h"
#include "atlas/db/transaction.h"
#include "generated/db/character_items_row.h"
#include "generated/db/characters_row.h"
#include "tests/optional_assert.h"

// 경계를 넘는 요청은 많아야 한 번 실행된다. 결과는 나중에 복구 가능
// 분산 트랜잭션은 범위 밖. 2PC 도 Saga 도 없다
// 멱등 기록은 메모리라 재시작을 못 넘긴다. 케이스 4 가 그것을 단언한다
// [AD 5.2] 시험 대상은 패킷 교환이 아니다
// Received -> Persisted -> Responded 상태 기계와 송신자가 고른 키다

namespace
{

using atlas::Ctx;
using atlas::DbConnectionConfig;
using atlas::DbRow;
using atlas::DbValue;
using atlas::Duration;
using atlas::IdempotencyStore;
using atlas::Int64;
using atlas::RequestAdmission;
using atlas::RequestAdmissionResult;
using atlas::RequestKey;
using atlas::RequestKeyValue;
using atlas::RequestState;
using atlas::SysTime;
using atlas::TxState;
using atlas::UInt16;
using atlas::UInt64;

// 다른 스위트도 시드도 안 쓰는 server_id. 30001 은 db_runtime_test.cpp
constexpr UInt64 kTxServerId = 30002;
constexpr UInt64 kTxCharacterId = 5150;
constexpr UInt64 kTxAccountUid = 9002;
constexpr UInt64 kBaseExp = 1000;

// 진행 중인 중복이 끝나기를 기다려 주는 시간. 넘으면 "진행 중" 응답
constexpr atlas::Seconds kWaitBudget{ 5 };
// 여기 어떤 요청보다 넉넉히 김. TTL 은 메모리 상한이지 실행 타임아웃이 아님
constexpr atlas::Seconds kRecordTtl{ 60 };

// 이 계층의 다른 것들처럼 고정 텍스트 + 플레이스홀더. 문자열 연결 없음
constexpr std::string_view kDeleteCharactersSql = "DELETE FROM `characters` WHERE `server_id` = ?";
constexpr std::string_view kDeleteRequestRowsSql =
    "DELETE FROM `character_items` WHERE `server_id` = ?";
constexpr std::string_view kSelectRequestRowsSql =
    "SELECT `item_uid` FROM `character_items` WHERE `server_id` = ? AND `character_id` = ?";

std::optional< DbConnectionConfig > LoadTxConfig()
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
    // game_equip_test.cpp 참고. 서버와 같은 TLS 조건으로 접속
    config.tls_no_verify = secrets.db_tls_no_verify;
#if defined( ATLAS_MARIADB_PLUGIN_DIR )
    config.plugin_directory = ATLAS_MARIADB_PLUGIN_DIR;
#endif
    return config;
}

// 스키마의 DATETIME 에 소수부가 없어 마이크로초가 든 값은 왕복을 못 넘김
SysTime NowToWholeSecond()
{
    return std::chrono::floor< std::chrono::seconds >( atlas::SysClock::now() );
}

// =============================================================================
// 요청이 다루는 지속 상태
// =============================================================================

// characters 한 행이 카운터를 든다
// 완료된 요청마다 PK 가 곧 요청 키인 character_items 한 행이 남는다
// 커밋의 유일한 지속 증거

DbRow ReadCharacterRow( atlas::Connection& connection )
{
    const std::array< DbValue, 2 > parameters{ DbValue{ kTxServerId }, DbValue{ kTxCharacterId } };
    std::vector< DbRow > rows =
        connection.Prepare( atlas::generated::kCharactersSelectByPkSql ).Query( parameters );
    if ( rows.empty() )
    {
        throw std::runtime_error( "the fixture row is missing" );
    }
    return rows.front();
}

UInt64 ReadExp( atlas::Connection& connection )
{
    return std::get< UInt64 >(
        ReadCharacterRow( connection )[atlas::generated::kCharactersColExp] );
}

void WriteExp( atlas::Connection& connection, UInt64 exp )
{
    DbRow row = ReadCharacterRow( connection );
    row[atlas::generated::kCharactersColExp] = DbValue{ exp };

    // UPDATE 는 키 컬럼이 마지막에 바인딩됨. 컬럼 순서를 가정하는 대신 생성된 배열을 순회
    std::vector< DbValue > parameters;
    parameters.reserve( atlas::generated::kCharactersUpdateByPkBinding.size() );
    for ( const std::size_t column : atlas::generated::kCharactersUpdateByPkBinding )
    {
        parameters.push_back( row[column] );
    }
    connection.Prepare( atlas::generated::kCharactersUpdateByPkSql ).Execute( parameters );
}

void InsertRequestRow( atlas::Connection& connection, RequestKey key )
{
    const std::array< DbValue, 6 > parameters{
        DbValue{ kTxServerId }, DbValue{ kTxCharacterId }, DbValue{ RequestKeyValue( key ) },
        DbValue{ UInt64{ 1 } }, DbValue{ UInt64{ 1 } },    DbValue{ UInt64{ 0 } } };
    connection.Prepare( atlas::generated::kCharacterItemsInsertSql ).Execute( parameters );
}

void DeleteRequestRow( atlas::Connection& connection, RequestKey key )
{
    const std::array< DbValue, 3 > parameters{ DbValue{ kTxServerId }, DbValue{ kTxCharacterId },
                                               DbValue{ RequestKeyValue( key ) } };
    connection.Prepare( atlas::generated::kCharacterItemsDeleteByPkSql ).Execute( parameters );
}

bool RequestRowExists( atlas::Connection& connection, RequestKey key )
{
    const std::array< DbValue, 3 > parameters{ DbValue{ kTxServerId }, DbValue{ kTxCharacterId },
                                               DbValue{ RequestKeyValue( key ) } };
    return !connection.Prepare( atlas::generated::kCharacterItemsSelectByPkSql )
                .Query( parameters )
                .empty();
}

std::size_t CountRequestRows( atlas::Connection& connection )
{
    const std::array< DbValue, 2 > parameters{ DbValue{ kTxServerId }, DbValue{ kTxCharacterId } };
    return connection.Prepare( kSelectRequestRowsSql ).Query( parameters ).size();
}

std::string BuildResponse( UInt64 exp_after ) { return std::format( "applied:exp={}", exp_after ); }

// =============================================================================
// 시험 대상 - 요청 서버
// =============================================================================

// 라벨 붙은 요청을 많아야 한 번 처리하는 서버 하나
class RequestServer
{
public:
    struct Hooks
    {
        // 트랜잭션 안, 커밋 직전에 실행. 동시성 케이스가 필요로 하는 걸쇠
        std::function< void() > in_transaction{};
        // 커밋 이후의 외부 부수효과. throw 는 실패이며 보상 경로를 무장시킴
        std::function< void() > side_effect{};
        // "응답 유실" 과 "여기서 프로세스가 죽음" 의 주입점
        // 커밋은 지속되고 기록은 Persisted 인데 클라이언트는 듣지 못한다
        bool stop_before_response{ false };
    };

    struct Outcome
    {
        RequestAdmission admission{ RequestAdmission::Started };
        std::string response{};
    };

    RequestServer( const DbConnectionConfig& config, Duration record_ttl )
        : pool_( config, 2 ), store_( record_ttl )
    {
    }

    IdempotencyStore& Store() noexcept { return store_; }
    UInt64 Executions() const noexcept { return executions_.load(); }

    Outcome Handle( Ctx& ctx, UInt64 exp_delta, const Hooks& hooks )
    {
        const RequestAdmissionResult admitted = store_.Begin( ctx, kWaitBudget );
        if ( admitted.admission != RequestAdmission::Started )
        {
            // 기록된 결과를 바이트 그대로. 두 번째 실행도 두 번째 커밋도 없음
            return { .admission = admitted.admission, .response = admitted.result };
        }

        UInt64 exp_after = 0;
        {
            atlas::PooledConnection lease = Lease();
            try
            {
                Transact( ctx, *lease, exp_delta, hooks, exp_after );
            }
            catch ( ... )
            {
                // 롤백됐으니 요청은 없던 일
                // 키를 비우지 않으면 재전송이 아무도 만들지 않은 결과를 재생받는다
                store_.Abandon( ctx );
                throw;
            }
        }
        executions_.fetch_add( 1 );

        // 반드시 커밋 이후. 열린 트랜잭션에선 저장소가 거부해 순서가 강제됨
        std::string response = BuildResponse( exp_after );
        store_.MarkPersisted( ctx, response );

        if ( hooks.side_effect )
        {
            // 커밋을 지나면 롤백은 없음. 되돌리는 길은 역쓰기뿐
            atlas::PostCommitGuard undo(
                [&]
                {
                    Compensate( ctx, exp_delta );
                    store_.Abandon( ctx );
                } );
            hooks.side_effect();
            undo.Release();
        }

        if ( hooks.stop_before_response )
        {
            return { .admission = RequestAdmission::Started, .response = std::move( response ) };
        }
        store_.MarkResponded( ctx );
        return { .admission = RequestAdmission::Started, .response = std::move( response ) };
    }

    // 재시작 뒤 지속 상태로부터 기록을 재구성. 이 키로 커밋된 것이 없으면 false
    bool RecoverFromDurableState( Ctx& ctx )
    {
        atlas::PooledConnection lease = Lease();
        if ( !RequestRowExists( *lease, ctx.request_key ) )
        {
            return false;
        }
        // 재생이 아니라 재구성. 지속 상태가 답을 결정할 때만 충실하다
        // 그 밖의 것에 의존한 결과는 돌아오지 못한다
        store_.Seed( ctx, BuildResponse( ReadExp( *lease ) ) );
        return true;
    }

private:
    // optional 이 아니라 연결 자체를 반환. 풀 고갈은 여기서 throw
    // 그래서 호출자는 언제나 존재하는 리스를 쥔다
    atlas::PooledConnection Lease()
    {
        std::optional< atlas::PooledConnection > lease = pool_.Acquire( atlas::Seconds{ 5 } );
        if ( !lease.has_value() )
        {
            throw std::runtime_error( "the connection pool was exhausted" );
        }
        return std::move( *lease );
    }

    void Transact( Ctx& ctx, atlas::Connection& connection, UInt64 exp_delta, const Hooks& hooks,
                   UInt64& exp_after )
    {
        atlas::Transaction transaction( connection, ctx );
        exp_after = ReadExp( connection ) + exp_delta;
        WriteExp( connection, exp_after );
        // 두 테이블, 한 트랜잭션. 카운터와 요청의 지속 증거는 함께 반영되거나 아니거나
        InsertRequestRow( connection, ctx.request_key );
        if ( hooks.in_transaction )
        {
            hooks.in_transaction();
        }
        transaction.Commit();
    }

    void Compensate( const Ctx& ctx, UInt64 exp_delta )
    {
        atlas::PooledConnection lease = Lease();
        // 자기만의 원장. 원래 ctx 의 Committed 를 덮으면 보상 대상이 지워짐
        Ctx compensation_ctx;
        compensation_ctx.trace_id = ctx.trace_id;
        compensation_ctx.request_key = ctx.request_key;

        atlas::Transaction transaction( *lease, compensation_ctx );
        WriteExp( *lease, ReadExp( *lease ) - exp_delta );
        DeleteRequestRow( *lease, ctx.request_key );
        transaction.Commit();
    }

    atlas::ConnectionPool pool_;
    IdempotencyStore store_;
    std::atomic< UInt64 > executions_{ 0 };
};

class TxAtomicityTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::optional< DbConnectionConfig > loaded = LoadTxConfig();
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
        Seed( *probe_ );
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
        const std::array< DbValue, 1 > parameters{ DbValue{ kTxServerId } };
        connection.Prepare( kDeleteRequestRowsSql ).Execute( parameters );
        connection.Prepare( kDeleteCharactersSql ).Execute( parameters );
    }

    static void Seed( atlas::Connection& connection )
    {
        const std::array< DbValue, 10 > parameters{
            DbValue{ kTxServerId },        DbValue{ kTxCharacterId },
            DbValue{ kTxAccountUid },      DbValue{ std::string{ "tx-atomicity" } },
            DbValue{ Int64{ 0 } },         DbValue{ Int64{ 0 } },
            DbValue{ UInt64{ 1 } },        DbValue{ kBaseExp },
            DbValue{ NowToWholeSecond() }, DbValue{ std::monostate{} } };
        connection.Prepare( atlas::generated::kCharactersInsertSql ).Execute( parameters );
    }

    DbConnectionConfig config_{};
    std::unique_ptr< atlas::Connection > probe_;
};

// =============================================================================
// 결함 주입 케이스 1 - 5
// =============================================================================

// 케이스 1. 응답이 유실됨
TEST_F( TxAtomicityTest, LostResponseIsReplayedFromTheRecordWithoutASecondExecution )
{
    RequestServer server( config_, kRecordTtl );
    const RequestKey key{ 0xA1 };

    Ctx first;
    first.trace_id = 5101;
    first.request_key = key;
    RequestServer::Hooks lose_it;
    lose_it.stop_before_response = true;
    const RequestServer::Outcome sent = server.Handle( first, 50, lose_it );

    // 커밋은 반영됐고 기록도 그렇게 말하지만 클라이언트는 듣지 못함
    EXPECT_EQ( first.tx_state, TxState::Committed );
    EXPECT_EQ( first.request_state, RequestState::Persisted );
    EXPECT_EQ( server.Executions(), 1U );

    // 재전송은 이쪽에서 새 중복과 구분 불가. 판가름하는 것은 키
    Ctx again;
    again.trace_id = 5102;
    again.request_key = key;
    const RequestServer::Outcome replayed = server.Handle( again, 50, RequestServer::Hooks{} );

    EXPECT_EQ( replayed.admission, RequestAdmission::Replay );
    EXPECT_EQ( replayed.response, sent.response );
    EXPECT_EQ( server.Executions(), 1U );

    atlas::Connection reader( config_ );
    EXPECT_EQ( ReadExp( reader ), kBaseExp + 50 );
    EXPECT_EQ( CountRequestRows( reader ), 1U );
}

// 케이스 2. 같은 요청의 사본 두 개가 동시에 도착
TEST_F( TxAtomicityTest, SimultaneousDuplicatesExecuteOnceAndAnswerIdentically )
{
    RequestServer server( config_, kRecordTtl );
    const RequestKey key{ 0xB2 };

    std::atomic< int > at_the_gate{ 0 };
    std::atomic< bool > threw{ false };
    std::array< RequestServer::Outcome, 2 > outcomes;
    std::array< Ctx, 2 > contexts;
    contexts[0].trace_id = 5201;
    contexts[1].trace_id = 5202;
    contexts[0].request_key = key;
    contexts[1].request_key = key;

    // 빠져나가게 두지 않고 잡음. std::thread 를 벗어난 예외는 프로세스를 종료시킴
    auto arrive = [&]( std::size_t index )
    {
        at_the_gate.fetch_add( 1 );
        while ( at_the_gate.load() < 2 )
        {
            std::this_thread::yield();
        }
        try
        {
            outcomes[index] = server.Handle( contexts[index], 70, RequestServer::Hooks{} );
        }
        catch ( ... )
        {
            threw.store( true );
        }
    };

    std::thread left( arrive, 0 );
    std::thread right( arrive, 1 );
    left.join();
    right.join();

    ASSERT_FALSE( threw.load() );
    EXPECT_EQ( server.Executions(), 1U );
    // 소유자는 정확히 하나. 진 쪽은 Begin 안에서 막혀 있다 결과를 가져감
    const int started = static_cast< int >( outcomes[0].admission == RequestAdmission::Started ) +
                        static_cast< int >( outcomes[1].admission == RequestAdmission::Started );
    EXPECT_EQ( started, 1 );
    EXPECT_EQ( outcomes[0].response, outcomes[1].response );
    EXPECT_FALSE( outcomes[0].response.empty() );

    atlas::Connection reader( config_ );
    EXPECT_EQ( ReadExp( reader ), kBaseExp + 70 );
    EXPECT_EQ( CountRequestRows( reader ), 1U );
}

// 케이스 3. 첫 사본이 아직 도는 중에 타임아웃 재전송이 도착
TEST_F( TxAtomicityTest, RetransmitDuringExecutionIsToldInProgressAndNeverRunsASecondCopy )
{
    RequestServer server( config_, kRecordTtl );
    const RequestKey key{ 0xC3 };

    std::atomic< bool > inside{ false };
    std::atomic< bool > release{ false };
    std::atomic< bool > threw{ false };

    Ctx owner;
    owner.trace_id = 5301;
    owner.request_key = key;
    RequestServer::Outcome owner_outcome;

    RequestServer::Hooks hold;
    hold.in_transaction = [&]
    {
        inside.store( true );
        while ( !release.load() )
        {
            std::this_thread::yield();
        }
    };

    std::thread worker(
        [&]
        {
            try
            {
                owner_outcome = server.Handle( owner, 90, hold );
            }
            catch ( ... )
            {
                threw.store( true );
            }
            // 무슨 일이 있었든, 메인 스레드가 영원히 도는 것을 막음
            inside.store( true );
        } );
    while ( !inside.load() )
    {
        std::this_thread::yield();
    }

    // 여기서 join 까지 사이에 단언 금지. 실패한 단언은 본문에서 반환한다
    // 워커가 멈춰 선 채 반환하면 joinable 스레드가 남아 terminate 된다

    // 짧은 예산으로 묻는 재전송. "기다려" 가 아니라 "끝났나"
    Ctx probing;
    probing.trace_id = 5302;
    probing.request_key = key;
    const RequestAdmissionResult probe = server.Store().Begin( probing, atlas::Millis{ 100 } );
    const UInt64 executions_while_held = server.Executions();

    release.store( true );
    worker.join();

    ASSERT_FALSE( threw.load() );
    EXPECT_EQ( probe.admission, RequestAdmission::InProgress );
    EXPECT_EQ( probe.state, RequestState::Received );
    EXPECT_TRUE( probe.result.empty() );
    EXPECT_EQ( executions_while_held, 0U );
    ASSERT_EQ( server.Executions(), 1U );

    Ctx later;
    later.trace_id = 5303;
    later.request_key = key;
    const RequestServer::Outcome answered = server.Handle( later, 90, RequestServer::Hooks{} );
    EXPECT_EQ( answered.admission, RequestAdmission::Replay );
    EXPECT_EQ( answered.response, owner_outcome.response );
    EXPECT_EQ( server.Executions(), 1U );

    atlas::Connection reader( config_ );
    EXPECT_EQ( ReadExp( reader ), kBaseExp + 90 );
    EXPECT_EQ( CountRequestRows( reader ), 1U );
}

// 케이스 4. 커밋과 응답 사이에서 프로세스가 죽는다
// 커밋된 행은 진짜 지속. MySQL 이 받아들였고 그대로 있다
// 멱등 기록은 아니다. 이 프로세스의 unordered_map 이라 재시작하면 사라진다
// 모사한 것은 프로세스 메모리의 손실뿐. 시그널 처리와 소켓 정리는 범위 밖
// 증명하는 것은 상태 기계가 지속 상태에서 재개된다는 것
TEST_F( TxAtomicityTest, CrashAfterCommitResumesFromDurableStateAndLosesTheInMemoryRecord )
{
    const RequestKey key{ 0xD4 };
    std::string response_never_sent;

    {
        RequestServer dying( config_, kRecordTtl );
        Ctx ctx;
        ctx.trace_id = 5401;
        ctx.request_key = key;
        RequestServer::Hooks die_here;
        die_here.stop_before_response = true;

        response_never_sent = dying.Handle( ctx, 120, die_here ).response;
        EXPECT_EQ( ctx.tx_state, TxState::Committed );
        EXPECT_EQ( ctx.request_state, RequestState::Persisted );

        const std::optional< RequestState > recorded = dying.Store().Find( key );
        ATLAS_ASSERT_HAS_VALUE( recorded );
        EXPECT_EQ( *recorded, RequestState::Persisted );
    }  // 여기서 프로세스가 죽음

    RequestServer restarted( config_, kRecordTtl );

    // 살아남지 못한 절반. 얼버무리지 않고 단언
    EXPECT_FALSE( restarted.Store().Find( key ).has_value() );
    EXPECT_EQ( restarted.Store().Size(), 0U );

    // 살아남은 절반. 커밋된 행과 그것이 담은 불변식
    atlas::Connection reader( config_ );
    EXPECT_EQ( ReadExp( reader ), kBaseExp + 120 );
    EXPECT_EQ( CountRequestRows( reader ), 1U );

    // 재개. 복구가 지속 증거를 찾아 Persisted 를 심고, 핸들러는 실행 없이 기록에서 답함
    Ctx retransmit;
    retransmit.trace_id = 5402;
    retransmit.request_key = key;
    ASSERT_TRUE( restarted.RecoverFromDurableState( retransmit ) );
    EXPECT_EQ( retransmit.request_state, RequestState::Persisted );

    const RequestServer::Outcome answered =
        restarted.Handle( retransmit, 120, RequestServer::Hooks{} );
    EXPECT_EQ( answered.admission, RequestAdmission::Replay );
    EXPECT_EQ( answered.response, response_never_sent );
    EXPECT_EQ( restarted.Executions(), 0U );

    EXPECT_EQ( ReadExp( reader ), kBaseExp + 120 );
    EXPECT_EQ( CountRequestRows( reader ), 1U );
}

// 보상 경로. 커밋 이후 단계가 실패
TEST_F( TxAtomicityTest, PostCommitSideEffectFailureIsUndoneByAnInverseWrite )
{
    RequestServer server( config_, kRecordTtl );
    const RequestKey key{ 0xE5 };

    Ctx ctx;
    ctx.trace_id = 5501;
    ctx.request_key = key;
    RequestServer::Hooks failing;
    failing.side_effect = []
    { throw std::runtime_error( "the downstream step refused the committed result" ); };

    // 커밋은 이미 성공. 역을 쓰는 두 번째 커밋이 최종 상태만 되사 옴
    EXPECT_THROW( server.Handle( ctx, 200, failing ), std::runtime_error );
    EXPECT_EQ( ctx.tx_state, TxState::Committed );
    EXPECT_EQ( server.Executions(), 1U );

    atlas::Connection reader( config_ );
    EXPECT_EQ( ReadExp( reader ), kBaseExp );
    EXPECT_EQ( CountRequestRows( reader ), 0U );

    // 결국 없던 일이라 키는 비고 재전송은 실행됨. 되돌려진 결과 재생이 더 나쁨
    EXPECT_FALSE( server.Store().Find( key ).has_value() );
    Ctx retry;
    retry.trace_id = 5502;
    retry.request_key = key;
    const RequestServer::Outcome retried = server.Handle( retry, 200, RequestServer::Hooks{} );
    EXPECT_EQ( retried.admission, RequestAdmission::Started );
    EXPECT_EQ( server.Executions(), 2U );
    EXPECT_EQ( ReadExp( reader ), kBaseExp + 200 );
    EXPECT_EQ( CountRequestRows( reader ), 1U );
}

// =============================================================================
// 순서와 TTL - DB 불필요, 항상 실행
// =============================================================================

TEST( IdempotencyOrderingTest, RecordingAResultWhileTheTransactionIsStillOpenIsRejected )
{
    IdempotencyStore store( kRecordTtl );
    Ctx ctx;
    ctx.request_key = RequestKey{ 0xF1 };
    ASSERT_EQ( store.Begin( ctx, atlas::Millis{ 0 } ).admission, RequestAdmission::Started );

    // "커밋 전에 응답" 을 기계적으로 잡는다
    // DB 가 끝내 받지 않을 숫자를 클라이언트가 쥐게 되는 것이 틀린 이유
    ctx.tx_state = TxState::Active;
    EXPECT_THROW( store.MarkPersisted( ctx, "premature" ), atlas::Exception );

    ctx.tx_state = TxState::Committed;
    store.MarkPersisted( ctx, "after the commit" );
    EXPECT_EQ( ctx.request_state, RequestState::Persisted );
}

TEST( IdempotencyOrderingTest, TheStateMachineCannotSkipPersisted )
{
    IdempotencyStore store( kRecordTtl );
    Ctx ctx;
    ctx.request_key = RequestKey{ 0xF2 };
    ASSERT_EQ( store.Begin( ctx, atlas::Millis{ 0 } ).admission, RequestAdmission::Started );

    EXPECT_THROW( store.MarkResponded( ctx ), atlas::Exception );

    ctx.tx_state = TxState::Committed;
    store.MarkPersisted( ctx, "result" );
    store.MarkResponded( ctx );
    EXPECT_EQ( ctx.request_state, RequestState::Responded );
}

TEST( IdempotencyOrderingTest, AnUnlabelledRequestIsRefusedRatherThanSharingOneKey )
{
    IdempotencyStore store( kRecordTtl );
    Ctx ctx;  // request_key 미설정
    EXPECT_THROW( store.Begin( ctx, atlas::Millis{ 0 } ), atlas::Exception );
}

TEST( IdempotencyOrderingTest, ExpiredRecordsAreSweptAndFreeTheirKey )
{
    IdempotencyStore store( atlas::Millis{ 20 } );
    Ctx ctx;
    ctx.request_key = RequestKey{ 0xF3 };
    ASSERT_EQ( store.Begin( ctx, atlas::Millis{ 0 } ).admission, RequestAdmission::Started );
    ctx.tx_state = TxState::Committed;
    store.MarkPersisted( ctx, "result" );
    ASSERT_EQ( store.Size(), 1U );

    std::this_thread::sleep_for( atlas::Millis{ 60 } );

    EXPECT_FALSE( store.Find( RequestKey{ 0xF3 } ).has_value() );
    EXPECT_EQ( store.PurgeExpired(), 1U );
    EXPECT_EQ( store.Size(), 0U );
}

}  // namespace
