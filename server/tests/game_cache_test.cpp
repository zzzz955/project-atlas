// =============================================================================
// [AD 10.2] 캐시 축. exp 랭킹, 캐릭터 읽기 캐시, 그리고 둘의 존재 근거
// =============================================================================

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "atlas/config/secret_config.h"
#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/db/prepared_statement.h"
#include "atlas/net/io_runner.h"
#include "atlas/net/net_types.h"
#include "atlas/proto/frame.h"
#include "atlas/redis/connection.h"
#include "atlas/redis/redis_runner.h"
#include "game/character.h"
#include "game/character_cache.h"
#include "game/handlers.h"
#include "game/inventory.h"
#include "game/ranking.h"
#include "generated/db/characters_row.h"
#include "generated/pkt/pkt_codec.h"
#include "tests/optional_assert.h"

// [AD 10.2] 주장하는 것
// - 랭킹은 characters.exp 내림차순. 읽기 캐시는 miss 후 hit
// - 쓰기는 사본을 깁지 않고 버린다. 무효화가 유일한 답
// - Redis 가 죽어도 캐릭터는 로드된다. 커밋 없으면 랭킹도 없다
// pub/sub, 축출, 클러스터, 센티널은 범위 밖. 안 닿으면 크게 SKIP

namespace
{

using atlas::Byte;
using atlas::DbConnectionConfig;
using atlas::DbRow;
using atlas::DbValue;
using atlas::Int64;
using atlas::SysTime;
using atlas::UInt16;
using atlas::UInt32;
using atlas::UInt64;
using atlas::UInt8;
using atlas_demo::CachedCharacter;
using atlas_demo::EquipSlot;
using atlas_demo::RankEntry;

// 다른 스위트가 안 쓰는 server_id. 30001 은 db, 30002 는 tx, 30003 은 equip
constexpr UInt16 kServerId = 30004;

constexpr UInt64 kAccountUid = 9400;
constexpr UInt64 kCharacterLow = 6401;
constexpr UInt64 kCharacterMid = 6402;
constexpr UInt64 kCharacterHigh = 6403;

// 일부러 삽입 순서와 다름. 넣은 순서 그대로 돌려주는 저장소로는 통과 불가
constexpr UInt64 kExpLow = 100;
constexpr UInt64 kExpMid = 5000;
constexpr UInt64 kExpHigh = 90000;

constexpr UInt64 kItemHeld = 7401;

constexpr std::string_view kDeleteItemsSql = "DELETE FROM `character_items` WHERE `server_id` = ?";
constexpr std::string_view kDeleteCharactersSql = "DELETE FROM `characters` WHERE `server_id` = ?";

// 아무도 듣지 않는 포트로 "Redis 다운" 주입. mock 은 mock 을 증명할 뿐
constexpr UInt16 kDeadRedisPort = 1;

std::optional< DbConnectionConfig > LoadDbConfig()
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
    config.tls_no_verify = secrets.db_tls_no_verify;
#if defined( ATLAS_MARIADB_PLUGIN_DIR )
    config.plugin_directory = ATLAS_MARIADB_PLUGIN_DIR;
#endif
    return config;
}

atlas::RedisConnectionConfig LoadRedisConfig()
{
    const atlas::SecretConfig secrets = atlas::SecretConfig::FromEnvironment();
    atlas::RedisConnectionConfig config;
    config.host = secrets.redis_host;
    config.port = secrets.redis_port == 0 ? UInt16{ 6379 } : secrets.redis_port;
    config.password = secrets.redis_password;
    // 게이트는 호스트에서 돌면서 ATLAS_DB_HOST 를 loopback 으로 바꾼다
    // Redis 엔 아무도 그걸 해 주지 않아 이 스위트가 직접 한다
    // 미설정도 같은 가지. compose 가 6379 를 호스트에 펴기 때문
    if ( config.host.empty() || config.host == "redis" )
    {
        config.host = "127.0.0.1";
    }
    return config;
}

SysTime NowToWholeSecond()
{
    return std::chrono::floor< std::chrono::seconds >( atlas::SysClock::now() );
}

template < class Predicate >
bool WaitUntil( Predicate predicate )
{
    const atlas::TimePoint deadline = atlas::Clock::now() + atlas::Seconds{ 10 };
    while ( !predicate() )
    {
        if ( atlas::Clock::now() > deadline )
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

void InsertCharacter( atlas::Connection& connection, UInt64 character_id, const std::string& name,
                      UInt64 exp )
{
    const std::array< DbValue, 10 > parameters{ DbValue{ static_cast< UInt64 >( kServerId ) },
                                                DbValue{ character_id },
                                                DbValue{ kAccountUid },
                                                DbValue{ name },
                                                DbValue{ Int64{ 0 } },
                                                DbValue{ Int64{ 0 } },
                                                DbValue{ UInt64{ 1 } },
                                                DbValue{ exp },
                                                DbValue{ NowToWholeSecond() },
                                                DbValue{ std::monostate{} } };
    connection.Prepare( atlas::generated::kCharactersInsertSql ).Execute( parameters );
}

void InsertItem( atlas::Connection& connection, atlas::CharacterId character_id, UInt64 item_uid,
                 UInt32 item_id, EquipSlot slot )
{
    const std::array< DbValue, 6 > parameters{
        DbValue{ static_cast< UInt64 >( kServerId ) },
        DbValue{ atlas::IdValue( character_id ) },
        DbValue{ item_uid },
        DbValue{ static_cast< UInt64 >( item_id ) },
        DbValue{ UInt64{ 1 } },
        DbValue{ static_cast< UInt64 >( static_cast< UInt8 >( slot ) ) } };
    connection.Prepare( atlas::generated::kCharacterItemsInsertSql ).Execute( parameters );
}

UInt8 SlotOf( atlas::Connection& connection, UInt64 item_uid )
{
    const std::array< DbValue, 2 > parameters{ DbValue{ static_cast< UInt64 >( kServerId ) },
                                               DbValue{ item_uid } };
    const std::vector< DbRow > rows =
        connection.Prepare( atlas_demo::kSelectItemByUidSql ).Query( parameters );
    if ( rows.empty() )
    {
        return 0xFF;
    }
    return atlas_demo::CharacterItemsRowFromDb( rows.front() ).equip_slot_;
}

// =============================================================================
// [AD 8.1] 실제 소켓, 실제 프레임 형식을 쓰는 헬퍼
// =============================================================================

// 공유하지 않고 복제. unity build 에선 한 타깃이 한 TU 라 익명 ns 가 충돌

class TestClient
{
public:
    explicit TestClient( const atlas::Endpoint& endpoint ) : socket_( io_context_ )
    {
        socket_.connect( endpoint );
    }

    TestClient( const TestClient& ) = delete;
    TestClient& operator=( const TestClient& ) = delete;
    TestClient( TestClient&& ) = delete;
    TestClient& operator=( TestClient&& ) = delete;

    ~TestClient()
    {
        atlas::ErrorCode ignored;
        std::ignore = socket_.shutdown( atlas::Socket::shutdown_both, ignored );
        std::ignore = socket_.close( ignored );
    }

    void Send( UInt16 opcode, const std::vector< Byte >& payload )
    {
        std::vector< Byte > wire;
        ++send_seq_;
        ASSERT_TRUE( atlas::EncodeFrame( wire, opcode, send_seq_, payload ) );
        atlas::asio::write( socket_, atlas::asio::buffer( wire ) );
    }

    atlas::Frame Receive()
    {
        std::vector< Byte > header( atlas::kFrameHeaderSize );
        atlas::asio::read( socket_, atlas::asio::buffer( header ) );
        atlas::FrameHeader decoded;
        EXPECT_TRUE( atlas::DecodeFrameHeader( header, decoded ) );

        std::vector< Byte > payload( decoded.length );
        if ( decoded.length != 0 )
        {
            atlas::asio::read( socket_, atlas::asio::buffer( payload ) );
        }
        EXPECT_EQ( atlas::FrameChecksum( decoded.opcode, decoded.seq, payload ), decoded.crc32 );
        return atlas::Frame{ .opcode = decoded.opcode, .seq = decoded.seq, .payload = payload };
    }

private:
    atlas::IoContext io_context_;
    atlas::Socket socket_;
    UInt32 send_seq_{ 0 };
};

struct LoadedCharacter
{
    UInt8 result{ 0xFF };
    UInt64 character_id{ 0 };
    UInt64 exp{ 0 };
    std::string name{};
    std::vector< atlas_demo::Item > items{};
};

LoadedCharacter ParseLoadResponse( const atlas::Frame& frame )
{
    LoadedCharacter parsed;
    std::span< const Byte > cursor( frame.payload );
    EXPECT_TRUE( atlas::generated::ReadLe( cursor, parsed.result ) );
    if ( parsed.result != static_cast< UInt8 >( atlas_demo::LoadResult::Ok ) )
    {
        return parsed;
    }

    UInt64 account_uid = 0;
    atlas::Int32 pos_x = 0;
    atlas::Int32 pos_y = 0;
    UInt16 level = 0;
    UInt16 count = 0;
    EXPECT_TRUE( atlas::generated::ReadLe( cursor, parsed.character_id ) );
    EXPECT_TRUE( atlas::generated::ReadLe( cursor, account_uid ) );
    EXPECT_TRUE( atlas::generated::ReadUtf8( cursor, parsed.name ) );
    EXPECT_TRUE( atlas::generated::ReadLe( cursor, pos_x ) );
    EXPECT_TRUE( atlas::generated::ReadLe( cursor, pos_y ) );
    EXPECT_TRUE( atlas::generated::ReadLe( cursor, level ) );
    EXPECT_TRUE( atlas::generated::ReadLe( cursor, parsed.exp ) );
    EXPECT_TRUE( atlas::generated::ReadLe( cursor, count ) );
    for ( UInt16 index = 0; index < count; ++index )
    {
        atlas_demo::Item entry;
        UInt8 slot = 0;
        EXPECT_TRUE( atlas::generated::ReadLe( cursor, entry.item_uid ) );
        EXPECT_TRUE( atlas::generated::ReadLe( cursor, entry.item_id ) );
        EXPECT_TRUE( atlas::generated::ReadLe( cursor, entry.stack_count ) );
        EXPECT_TRUE( atlas::generated::ReadLe( cursor, slot ) );
        entry.slot = static_cast< EquipSlot >( slot );
        parsed.items.push_back( entry );
    }
    return parsed;
}

std::vector< Byte > LoadRequestPayload( UInt64 character_id )
{
    std::vector< Byte > payload;
    atlas::generated::WriteLe( payload, character_id );
    return payload;
}

std::vector< Byte > EquipRequestPayload( UInt64 item_uid, EquipSlot slot )
{
    std::vector< Byte > payload;
    atlas::generated::WriteLe( payload, item_uid );
    atlas::generated::WriteLe( payload, static_cast< UInt8 >( slot ) );
    return payload;
}

const atlas_demo::Item* FindItem( const LoadedCharacter& loaded, UInt64 item_uid )
{
    for ( const atlas_demo::Item& entry : loaded.items )
    {
        if ( entry.item_uid == item_uid )
        {
            return &entry;
        }
    }
    return nullptr;
}

atlas_demo::GameServer::Options TestServerOptions()
{
    atlas_demo::GameServer::Options options;
    // any 주소가 아닌 loopback, 포트 0. 병렬 ctest 끼리 충돌 방지
    options.endpoint = atlas::Endpoint( atlas::asio::ip::address_v4::loopback(), 0 );
    options.server_id = kServerId;
    options.io_workers = 2;
    options.db_pool_size = 2;
    options.db_threads = 2;
    return options;
}

// =============================================================================
// 픽스처
// =============================================================================

class GameCacheTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::optional< DbConnectionConfig > loaded = LoadDbConfig();
        if ( !loaded.has_value() )
        {
            GTEST_SKIP() << "SKIPPED: no database configured. Set ATLAS_DB_HOST / ATLAS_DB_NAME / "
                            "ATLAS_DB_USER (and ATLAS_DB_PASSWORD) and start MySQL with "
                            "`docker compose --env-file server/.env up -d mysql`.";
        }
        db_config_ = *loaded;

        try
        {
            probe_ = std::make_unique< atlas::Connection >( db_config_ );
        }
        catch ( const std::exception& refused )
        {
            GTEST_SKIP() << "SKIPPED: ATLAS_DB_HOST is set but the database refused the "
                            "connection: "
                         << refused.what();
        }
        Cleanup( *probe_ );
        Seed( *probe_ );

        redis_config_ = LoadRedisConfig();
        io_runner_ = std::make_unique< atlas::IoRunner >( 1 );
        io_runner_->Start();
        redis_ = std::make_unique< atlas::RedisConnection >( io_runner_->Context(), redis_config_ );
        redis_->Start();
        if ( !redis_->WaitUntilReady( atlas::Seconds{ 5 } ) )
        {
            GTEST_SKIP() << "SKIPPED: no Redis reachable at " << redis_config_.host << ":"
                         << redis_config_.port
                         << ". Start it with `docker compose --env-file server/.env up -d redis`.";
        }

        runner_ = std::make_unique< atlas::RedisRunner >( *redis_ );
        ranking_ = std::make_unique< atlas_demo::ExpRanking >( *runner_ );
        cache_ = std::make_unique< atlas_demo::CharacterCache >( *runner_ );
        ClearRedisKeys();
    }

    void TearDown() override
    {
        if ( probe_ )
        {
            Cleanup( *probe_ );
        }
        if ( runner_ )
        {
            ClearRedisKeys();
        }
        cache_.reset();
        ranking_.reset();
        runner_.reset();
        if ( redis_ )
        {
            // 연결은 항상 io_context 보다 먼저 멈춘다
            // run 루프가 미결 작업이라 work guard 를 놓아도 드레인되지 않는다
            redis_->Stop();
            redis_.reset();
        }
        if ( io_runner_ )
        {
            io_runner_->Stop();
            io_runner_.reset();
        }
    }

    static void Cleanup( atlas::Connection& connection )
    {
        const std::array< DbValue, 1 > parameters{ DbValue{ static_cast< UInt64 >( kServerId ) } };
        connection.Prepare( kDeleteItemsSql ).Execute( parameters );
        connection.Prepare( kDeleteCharactersSql ).Execute( parameters );
    }

    static void Seed( atlas::Connection& connection )
    {
        InsertCharacter( connection, kCharacterLow, "atlas-low", kExpLow );
        InsertCharacter( connection, kCharacterMid, "atlas-mid", kExpMid );
        InsertCharacter( connection, kCharacterHigh, "atlas-high", kExpHigh );
        // 1001 은 item.csv 의 Rusty Sword, slot 1
        // [AD 8.2] 3계층이 정적 데이터에 없는 item_id 를 거부한다
        // 임의 번호를 쓰면 아래 equip 이 전부 거부된다
        InsertItem( connection, static_cast< atlas::CharacterId >( kCharacterMid ), kItemHeld, 1001,
                    EquipSlot::None );
    }

    void ClearRedisKeys()
    {
        RunCommand( atlas::RedisCommand{
            .verb = "DEL",
            .args = { atlas_demo::ExpRankingKey( kServerId ),
                      atlas_demo::CharacterCacheKey(
                          kServerId, static_cast< atlas::CharacterId >( kCharacterLow ) ),
                      atlas_demo::CharacterCacheKey(
                          kServerId, static_cast< atlas::CharacterId >( kCharacterMid ) ),
                      atlas_demo::CharacterCacheKey(
                          kServerId, static_cast< atlas::CharacterId >( kCharacterHigh ) ) } } );
    }

    // 운영 경로는 전부 비동기
    // 완료 콜백 안에서 실패한 EXPECT 는 보고할 곳이 없다
    // 그래서 async API 를 몰고 테스트 스레드에서 기다린다
    atlas::RedisResult RunCommand( const atlas::RedisCommand& command )
    {
        auto answered = std::make_shared< std::promise< atlas::RedisResult > >();
        std::future< atlas::RedisResult > pending = answered->get_future();
        runner_->Submit( atlas::Ctx{}, command,
                         [answered]( const atlas::Ctx&, const atlas::RedisResult& value )
                         { answered->set_value( value ); } );
        if ( pending.wait_for( atlas::Seconds{ 5 } ) != std::future_status::ready )
        {
            ADD_FAILURE() << "a redis command never completed";
            return atlas::RedisResult{};
        }
        return pending.get();
    }

    std::optional< CachedCharacter > GetCached( UInt64 character_id )
    {
        auto answered = std::make_shared< std::promise< std::optional< CachedCharacter > > >();
        std::future< std::optional< CachedCharacter > > pending = answered->get_future();
        cache_->Get( atlas::Ctx{}, kServerId, static_cast< atlas::CharacterId >( character_id ),
                     [answered]( std::optional< CachedCharacter > value )
                     { answered->set_value( std::move( value ) ); } );
        if ( pending.wait_for( atlas::Seconds{ 5 } ) != std::future_status::ready )
        {
            ADD_FAILURE() << "a cache lookup never completed";
            return std::nullopt;
        }
        return pending.get();
    }

    std::optional< std::vector< RankEntry > > TopRanked( UInt16 count )
    {
        auto answered =
            std::make_shared< std::promise< std::optional< std::vector< RankEntry > > > >();
        std::future< std::optional< std::vector< RankEntry > > > pending = answered->get_future();
        ranking_->Top(
            atlas::Ctx{}, kServerId, count,
            [answered]( bool ok, const std::vector< RankEntry >& entries )
            {
                answered->set_value( ok ? std::optional< std::vector< RankEntry > >{ entries }
                                        : std::nullopt );
            } );
        if ( pending.wait_for( atlas::Seconds{ 5 } ) != std::future_status::ready )
        {
            ADD_FAILURE() << "a ranking query never completed";
            return std::nullopt;
        }
        return pending.get();
    }

    // 실제 콜드 로드를 테스트 스레드에서. 아래 랭킹 케이스의 커밋 후 ZADD 출처
    atlas_demo::CharacterSnapshot ColdLoad( UInt64 character_id )
    {
        atlas_demo::CharacterSnapshot snapshot;
        atlas::Ctx ctx;
        atlas_demo::LoadCharacterOnDbThread( ctx, *probe_, kServerId,
                                             static_cast< atlas::CharacterId >( character_id ),
                                             snapshot, ranking_.get() );
        return snapshot;
    }

    DbConnectionConfig db_config_{};
    atlas::RedisConnectionConfig redis_config_{};
    std::unique_ptr< atlas::Connection > probe_;
    std::unique_ptr< atlas::IoRunner > io_runner_;
    std::unique_ptr< atlas::RedisConnection > redis_;
    std::unique_ptr< atlas::RedisRunner > runner_;
    std::unique_ptr< atlas_demo::ExpRanking > ranking_;
    std::unique_ptr< atlas_demo::CharacterCache > cache_;
};

// =============================================================================
// Redis 축 시나리오. 케이스 1 - 5
// =============================================================================

TEST_F( GameCacheTest, TheRankingReturnsTheTopCharactersInDescendingExp )
{
    // 로드마다 커밋 후에만 기록하므로 셋 뒤엔 정렬 집합이 DB 와 정확히 같음
    ASSERT_EQ( ColdLoad( kCharacterLow ).result, atlas_demo::LoadResult::Ok );
    ASSERT_EQ( ColdLoad( kCharacterHigh ).result, atlas_demo::LoadResult::Ok );
    ASSERT_EQ( ColdLoad( kCharacterMid ).result, atlas_demo::LoadResult::Ok );

    const std::optional< std::vector< RankEntry > > top = TopRanked( 10 );
    ATLAS_ASSERT_HAS_VALUE( top );
    ASSERT_EQ( top->size(), std::size_t{ 3 } );

    // 내림차순. 위 삽입 순서는 low, HIGH, mid 라 그대로 재생하면 실패
    EXPECT_EQ( ( *top )[0].character_id, static_cast< atlas::CharacterId >( kCharacterHigh ) );
    EXPECT_EQ( ( *top )[0].exp, kExpHigh );
    EXPECT_EQ( ( *top )[1].character_id, static_cast< atlas::CharacterId >( kCharacterMid ) );
    EXPECT_EQ( ( *top )[1].exp, kExpMid );
    EXPECT_EQ( ( *top )[2].character_id, static_cast< atlas::CharacterId >( kCharacterLow ) );
    EXPECT_EQ( ( *top )[2].exp, kExpLow );

    const std::optional< std::vector< RankEntry > > two = TopRanked( 2 );
    ATLAS_ASSERT_HAS_VALUE( two );
    EXPECT_EQ( two->size(), std::size_t{ 2 } );
}

// 케이스 2. miss 뒤 hit, 저장한 값이 왕복을 넘김
TEST_F( GameCacheTest, TheCacheMissesUntilItIsFilledAndThenReturnsTheSameState )
{
    EXPECT_FALSE( GetCached( kCharacterMid ).has_value() );

    const atlas_demo::CharacterSnapshot loaded = ColdLoad( kCharacterMid );
    ASSERT_EQ( loaded.result, atlas_demo::LoadResult::Ok );
    cache_->Put( atlas::Ctx{}, kServerId,
                 CachedCharacter{ .character = loaded.character, .items = loaded.items } );

    const std::optional< CachedCharacter > hit = GetCached( kCharacterMid );
    ATLAS_ASSERT_HAS_VALUE( hit );
    EXPECT_EQ( hit->character.character_id_, loaded.character.character_id_ );
    EXPECT_EQ( hit->character.account_uid_, loaded.character.account_uid_ );
    EXPECT_EQ( hit->character.name_, "atlas-mid" );
    EXPECT_EQ( hit->character.exp_, kExpMid );
    // nullable 컬럼은 값뿐 아니라 존재 여부까지 유지
    EXPECT_EQ( hit->character.last_login_at_.has_value(),
               loaded.character.last_login_at_.has_value() );
    ASSERT_EQ( hit->items.size(), std::size_t{ 1 } );
    EXPECT_EQ( hit->items.front().item_uid, kItemHeld );
    EXPECT_EQ( hit->items.front().slot, EquipSlot::None );

    // TTL 은 되읽을 필드가 없어 유무만 봄. EX 를 빠뜨리면 사본이 영원히 남음
    const atlas::RedisResult ttl = RunCommand( atlas::RedisCommand{
        .verb = "TTL",
        .args = { atlas_demo::CharacterCacheKey(
            kServerId, static_cast< atlas::CharacterId >( kCharacterMid ) ) } } );
    ASSERT_TRUE( ttl.ok );
    ASSERT_EQ( ttl.values.size(), std::size_t{ 1 } );
    // 가드 앞에서 참조로 묶음. front() 를 두 번 부르면 서로 다른 저장 위치
    const std::optional< std::string >& ttl_value = ttl.values.front();
    ATLAS_ASSERT_HAS_VALUE( ttl_value );
    EXPECT_NE( *ttl_value, "-1" );
}

// 케이스 3. Redis 가 죽어도 캐릭터는 로드된다. 이 파일의 핵심
// [AD 10.2] 실제 클라이언트, 실제 소켓, 실제 GAME 서버
// 캐시 엔드포인트만 아무도 듣지 않는 포트
// 그래도 Ok 면 사본은 임계 경로 위에 없다는 뜻
TEST_F( GameCacheTest, ACharacterLoadsFromTheDatabaseWhileRedisIsUnreachable )
{
    atlas::RedisConnectionConfig dead = redis_config_;
    dead.port = kDeadRedisPort;

    atlas_demo::GameServer server( TestServerOptions(), db_config_, dead );
    server.Start();

    {
        TestClient client( server.LocalEndpoint() );
        client.Send( atlas_demo::kOpCharacterLoadRequest, LoadRequestPayload( kCharacterMid ) );
        const LoadedCharacter loaded = ParseLoadResponse( client.Receive() );

        ASSERT_EQ( loaded.result, static_cast< UInt8 >( atlas_demo::LoadResult::Ok ) );
        EXPECT_EQ( loaded.character_id, kCharacterMid );
        EXPECT_EQ( loaded.name, "atlas-mid" );
        EXPECT_EQ( loaded.exp, kExpMid );
        ASSERT_EQ( loaded.items.size(), std::size_t{ 1 } );
        EXPECT_EQ( loaded.items.front().item_uid, kItemHeld );

        // 캐시가 없어도 쓰기 경로는 동작. 무효화 실패는 로그로만 남음
        client.Send( atlas_demo::kOpEquipRequest,
                     EquipRequestPayload( kItemHeld, EquipSlot::Weapon ) );
        const atlas::Frame equip_response = client.Receive();
        std::span< const Byte > cursor( equip_response.payload );
        UInt8 code = 0xFF;
        ASSERT_TRUE( atlas::generated::ReadLe( cursor, code ) );
        EXPECT_EQ( code, static_cast< UInt8 >( atlas_demo::EquipResult::Ok ) );
        EXPECT_EQ( SlotOf( *probe_, kItemHeld ), static_cast< UInt8 >( EquipSlot::Weapon ) );

        // 랭킹은 DB 형태가 없어 폴백 불가. 빈 순위표 대신 장애를 그대로 보고
        std::vector< Byte > request;
        atlas::generated::WriteLe( request, UInt16{ 10 } );
        client.Send( atlas_demo::kOpRankingRequest, request );
        const atlas::Frame ranking_response = client.Receive();
        ASSERT_EQ( ranking_response.opcode, atlas_demo::kOpRankingResponse );
        std::span< const Byte > ranking_cursor( ranking_response.payload );
        UInt8 ranking_code = 0xFF;
        ASSERT_TRUE( atlas::generated::ReadLe( ranking_cursor, ranking_code ) );
        EXPECT_EQ( ranking_code, static_cast< UInt8 >( atlas_demo::RankingResult::Unavailable ) );
    }

    ASSERT_TRUE( WaitUntil( [&] { return server.LiveSessionCount() == 0; } ) );
    server.Stop();
}

// 케이스 4. 쓰기가 사본을 버림. 실제 서버 경유
TEST_F( GameCacheTest, AnEquipInvalidatesTheCopySoTheNextLoadSeesTheNewSlot )
{
    atlas_demo::GameServer server( TestServerOptions(), db_config_, redis_config_ );
    server.Start();

    const std::string key = atlas_demo::CharacterCacheKey(
        kServerId, static_cast< atlas::CharacterId >( kCharacterMid ) );

    {
        TestClient client( server.LocalEndpoint() );

        client.Send( atlas_demo::kOpCharacterLoadRequest, LoadRequestPayload( kCharacterMid ) );
        ASSERT_EQ( ParseLoadResponse( client.Receive() ).result,
                   static_cast< UInt8 >( atlas_demo::LoadResult::Ok ) );
        ASSERT_TRUE( WaitUntil( [&] { return GetCached( kCharacterMid ).has_value(); } ) );

        client.Send( atlas_demo::kOpCharacterLoadRequest, LoadRequestPayload( kCharacterMid ) );
        const LoadedCharacter warm = ParseLoadResponse( client.Receive() );
        ASSERT_EQ( warm.result, static_cast< UInt8 >( atlas_demo::LoadResult::Ok ) );
        ASSERT_EQ( warm.items.size(), std::size_t{ 1 } );
        EXPECT_EQ( warm.items.front().slot, EquipSlot::None );

        // 쓰기. 사본은 이제 틀렸고 답은 깁는 것이 아니라 버리는 것
        // slot 이 Weapon 인 것은 item.csv 가 1001 에 준 슬롯이기 때문일 뿐
        client.Send( atlas_demo::kOpEquipRequest,
                     EquipRequestPayload( kItemHeld, EquipSlot::Weapon ) );
        const atlas::Frame equip_response = client.Receive();
        std::span< const Byte > cursor( equip_response.payload );
        UInt8 code = 0xFF;
        ASSERT_TRUE( atlas::generated::ReadLe( cursor, code ) );
        ASSERT_EQ( code, static_cast< UInt8 >( atlas_demo::EquipResult::Ok ) );
        ASSERT_TRUE( WaitUntil( [&] { return !GetCached( kCharacterMid ).has_value(); } ) );

        // 이 케이스의 핵심. 다음 독자는 새 상태를 본다
        // 쓰기를 견딘 캐시였다면 EquipSlot::None 을 답했을 것
        // DB 가 이미 뒤집은 사실을 클라이언트에 알리는 셈
        client.Send( atlas_demo::kOpCharacterLoadRequest, LoadRequestPayload( kCharacterMid ) );
        const LoadedCharacter refreshed = ParseLoadResponse( client.Receive() );
        ASSERT_EQ( refreshed.result, static_cast< UInt8 >( atlas_demo::LoadResult::Ok ) );
        ASSERT_NE( FindItem( refreshed, kItemHeld ), nullptr );
        EXPECT_EQ( FindItem( refreshed, kItemHeld )->slot, EquipSlot::Weapon );
    }

    ASSERT_TRUE( WaitUntil( [&] { return server.LiveSessionCount() == 0; } ) );
    server.Stop();
    EXPECT_TRUE( RunCommand( atlas::RedisCommand{ .verb = "DEL", .args = { key } } ).ok );
}

// 케이스 5. 커밋 없으면 랭킹도 없음. DB 가 받은 적 없는 값은 사본에 없어야 함
// 주입기가 커밋을 막고, 맨 아래 커밋하는 실행이 그 단언을 공허하지 않게 함
TEST_F( GameCacheTest, ARankingEntryIsNotWrittenWhenTheTransactionDoesNotCommit )
{
    const atlas::RedisCommand score{
        .verb = "ZSCORE",
        .args = { atlas_demo::ExpRankingKey( kServerId ), std::to_string( kCharacterHigh ) } };

    atlas_demo::CharacterSnapshot snapshot;
    atlas::Ctx ctx;
    EXPECT_THROW( atlas_demo::LoadCharacterOnDbThread(
                      ctx, *probe_, kServerId, static_cast< atlas::CharacterId >( kCharacterHigh ),
                      snapshot, ranking_.get(),
                      [] { throw std::runtime_error( "injected: the commit never happens" ); } ),
                  std::runtime_error );

    // [AD 10] 범위가 풀려 트랜잭션은 롤백되고 로드는 아무것도 못 만듦
    EXPECT_EQ( ctx.tx_state, atlas::TxState::RolledBack );
    EXPECT_NE( snapshot.result, atlas_demo::LoadResult::Ok );

    const atlas::RedisResult missing = RunCommand( score );
    ASSERT_TRUE( missing.ok );
    ASSERT_EQ( missing.values.size(), std::size_t{ 1 } );
    EXPECT_FALSE( missing.values.front().has_value() ) << "the ranking holds an uncommitted value";

    // 공허하지 않음. 주입기 없는 같은 호출은 사본을 실제로 씀
    ASSERT_EQ( ColdLoad( kCharacterHigh ).result, atlas_demo::LoadResult::Ok );
    const atlas::RedisResult present = RunCommand( score );
    ASSERT_TRUE( present.ok );
    ASSERT_EQ( present.values.size(), std::size_t{ 1 } );
    const std::optional< std::string >& score_value = present.values.front();
    ATLAS_ASSERT_HAS_VALUE( score_value );
    EXPECT_EQ( *score_value, std::to_string( kExpHigh ) );
}

// =============================================================================
// 인코딩 왕복. 저장소를 끼우지 않음
// =============================================================================

TEST( CharacterCacheCodec, EveryFieldSurvivesTheRoundTripAndGarbageDecodesAsAMiss )
{
    CachedCharacter value;
    value.character.server_id_ = kServerId;
    value.character.character_id_ = static_cast< atlas::CharacterId >( kCharacterMid );
    value.character.account_uid_ = static_cast< atlas::AccountId >( kAccountUid );
    value.character.name_ = "atlas-mid";
    value.character.pos_x_ = -12;
    value.character.pos_y_ = 34;
    value.character.level_ = 7;
    value.character.exp_ = kExpMid;
    value.character.created_at_ = NowToWholeSecond();
    value.character.last_login_at_ = std::nullopt;
    value.items.push_back( atlas_demo::Item{
        .item_uid = kItemHeld, .item_id = 21, .stack_count = 3, .slot = EquipSlot::Trinket } );

    const std::optional< CachedCharacter > decoded =
        atlas_demo::DecodeCachedCharacter( atlas_demo::EncodeCachedCharacter( value ) );
    ATLAS_ASSERT_HAS_VALUE( decoded );
    EXPECT_EQ( decoded->character, value.character );
    EXPECT_EQ( decoded->items, value.items );

    // 이 빌드가 못 읽는 값은 예외가 아니라 miss. DB 는 언제나 답할 수 있음
    EXPECT_FALSE( atlas_demo::DecodeCachedCharacter( "not an encoded character" ).has_value() );
    EXPECT_FALSE( atlas_demo::DecodeCachedCharacter( "" ).has_value() );
}

}  // namespace
