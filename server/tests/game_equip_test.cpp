// =============================================================================
// 데모 게임 도메인 + GAME 서버, 종단 검증
// =============================================================================

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
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
#include "atlas/net/net_types.h"
#include "atlas/proto/frame.h"
#include "atlas/proto/frame_reader.h"
#include "game/character.h"
#include "game/equip_service.h"
#include "game/handlers.h"
#include "game/inventory.h"
#include "game/npc.h"
#include "generated/db/character_items_row.h"
#include "generated/db/characters_row.h"
#include "generated/pkt/pkt_codec.h"

// 주장하는 것
// - "슬롯당 1개" 불변식이 세 번째 쓰기 실패를 견딘다. 부분 유니크 인덱스가 없다
// - 동시 equip 두 개가 직렬화된다. 슬롯은 둘을 담지 않는다
// - [AD 8.2] 3계층. 남의 아이템도, 없는 정의도, 금지된 슬롯도 거부
// - 실제 소켓으로 load -> equip -> 끊기 -> 재접속 후 장비 유지 확인

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
using atlas_demo::EquipResult;
using atlas_demo::EquipService;
using atlas_demo::EquipSlot;

// 다른 스위트도 시드 데이터도 안 쓰는 server_id. 30001 은 db, 30002 는 tx
constexpr UInt16 kServerId = 30003;

// 계정 1개에 캐릭터 2개. Account -> Character 상속으론 이 쌍을 표현 못 함
constexpr UInt64 kAccountUid = 9100;
constexpr UInt64 kCharacterA = 6100;
constexpr UInt64 kCharacterB = 6200;

// A 는 4개, B 는 1개 보유. B 의 것은 A 에게 거부됨
constexpr UInt64 kItemWorn = 7001;
constexpr UInt64 kItemSpare = 7002;
constexpr UInt64 kItemCharm = 7003;
constexpr UInt64 kItemUndefined = 7004;
constexpr UInt64 kItemOfOther = 7900;
constexpr UInt64 kItemMissing = 7999;

// [AD 8.2] item_id 는 item.csv 값이지 임의 숫자가 아니다
// 지어낸 id 를 심으면 정상 경로 대신 거부 경로를 시험하게 된다
constexpr UInt32 kRustySwordId = 1001;  // slot 1, 무기
constexpr UInt32 kIronSwordId = 1002;   // slot 1, 무기
constexpr UInt32 kLuckyCharmId = 3001;  // slot 3, 장신구
// 일부러 item.csv 에 없음. 적대적 클라이언트가 아니라 데이터 드리프트
constexpr UInt32 kUndefinedItemId = 9999;

constexpr std::string_view kDeleteItemsSql = "DELETE FROM `character_items` WHERE `server_id` = ?";
constexpr std::string_view kDeleteCharactersSql = "DELETE FROM `characters` WHERE `server_id` = ?";

std::optional< DbConnectionConfig > LoadGameConfig()
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
    // GAME 과 같은 변수에서 끌어낸 자세. 다르면 아무도 안 쓰는 연결을 시험
    config.tls_no_verify = secrets.db_tls_no_verify;
#if defined( ATLAS_MARIADB_PLUGIN_DIR )
    config.plugin_directory = ATLAS_MARIADB_PLUGIN_DIR;
#endif
    return config;
}

// 이 스키마의 DATETIME 에는 소수부가 없음
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

// =============================================================================
// DB 헬퍼 - 지속 상태 되읽기와 시딩
// =============================================================================

std::optional< atlas::generated::CharacterItemsRow > ReadItem( atlas::Connection& connection,
                                                               UInt64 item_uid )
{
    const std::array< DbValue, 2 > parameters{ DbValue{ static_cast< UInt64 >( kServerId ) },
                                               DbValue{ item_uid } };
    const std::vector< DbRow > rows =
        connection.Prepare( atlas_demo::kSelectItemByUidSql ).Query( parameters );
    if ( rows.empty() )
    {
        return std::nullopt;
    }
    return atlas_demo::CharacterItemsRowFromDb( rows.front() );
}

UInt8 SlotOf( atlas::Connection& connection, UInt64 item_uid )
{
    const std::optional< atlas::generated::CharacterItemsRow > row =
        ReadItem( connection, item_uid );
    return row.has_value() ? row->equip_slot_ : UInt8{ 0xFF };
}

std::size_t CountInSlot( atlas::Connection& connection, UInt64 character_id, EquipSlot slot )
{
    const std::array< DbValue, 3 > parameters{
        DbValue{ static_cast< UInt64 >( kServerId ) }, DbValue{ character_id },
        DbValue{ static_cast< UInt64 >( static_cast< UInt8 >( slot ) ) } };
    return connection.Prepare( atlas_demo::kSelectItemsInSlotSql ).Query( parameters ).size();
}

std::optional< SysTime > LastLoginOf( atlas::Connection& connection, UInt64 character_id )
{
    const std::array< DbValue, 2 > parameters{ DbValue{ static_cast< UInt64 >( kServerId ) },
                                               DbValue{ character_id } };
    const std::vector< DbRow > rows =
        connection.Prepare( atlas::generated::kCharactersSelectByPkSql ).Query( parameters );
    if ( rows.empty() )
    {
        return std::nullopt;
    }
    return atlas_demo::CharactersRowFromDb( rows.front() ).last_login_at_;
}

void InsertCharacter( atlas::Connection& connection, UInt64 character_id, const std::string& name )
{
    const std::array< DbValue, 10 > parameters{ DbValue{ static_cast< UInt64 >( kServerId ) },
                                                DbValue{ character_id },
                                                DbValue{ kAccountUid },
                                                DbValue{ name },
                                                DbValue{ Int64{ 0 } },
                                                DbValue{ Int64{ 0 } },
                                                DbValue{ UInt64{ 1 } },
                                                DbValue{ UInt64{ 0 } },
                                                DbValue{ NowToWholeSecond() },
                                                DbValue{ std::monostate{} } };
    connection.Prepare( atlas::generated::kCharactersInsertSql ).Execute( parameters );
}

// [CS 4.3] 소유자는 강타입 id, 아이템은 맨 UInt64. 같은 폭 인자가 뒤바뀌는 자리
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

// =============================================================================
// DB 픽스처와 케이스 1 - 7
// =============================================================================

class GameEquipTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::optional< DbConnectionConfig > loaded = LoadGameConfig();
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
        catch ( const std::exception& refused )
        {
            GTEST_SKIP() << "SKIPPED: ATLAS_DB_HOST is set but the database refused the "
                            "connection: "
                         << refused.what();
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
        const std::array< DbValue, 1 > parameters{ DbValue{ static_cast< UInt64 >( kServerId ) } };
        connection.Prepare( kDeleteItemsSql ).Execute( parameters );
        connection.Prepare( kDeleteCharactersSql ).Execute( parameters );
    }

    // A 는 kItemWorn 을 무기 슬롯에 차고 kItemSpare 를 놀린다
    // 슬롯이 찬 상태로 시작해야 쓰기 1 을 지나간다
    // 둘 다 무기여야 한 슬롯을 두고 경합한다
    static void Seed( atlas::Connection& connection )
    {
        InsertCharacter( connection, kCharacterA, "atlas-a" );
        InsertCharacter( connection, kCharacterB, "atlas-b" );
        InsertItem( connection, static_cast< atlas::CharacterId >( kCharacterA ), kItemWorn,
                    kRustySwordId, EquipSlot::Weapon );
        InsertItem( connection, static_cast< atlas::CharacterId >( kCharacterA ), kItemSpare,
                    kIronSwordId, EquipSlot::None );
        InsertItem( connection, static_cast< atlas::CharacterId >( kCharacterA ), kItemCharm,
                    kLuckyCharmId, EquipSlot::None );
        InsertItem( connection, static_cast< atlas::CharacterId >( kCharacterA ), kItemUndefined,
                    kUndefinedItemId, EquipSlot::None );
        InsertItem( connection, static_cast< atlas::CharacterId >( kCharacterB ), kItemOfOther,
                    kRustySwordId, EquipSlot::None );
    }

    static EquipService::Request RequestFor( UInt64 item_uid, EquipSlot slot,
                                             UInt64 character_id = kCharacterA )
    {
        return EquipService::Request{
            .server_id = kServerId,
            .character_id = static_cast< atlas::CharacterId >( character_id ),
            .item_uid = item_uid,
            .slot = slot };
    }

    DbConnectionConfig config_{};
    std::unique_ptr< atlas::Connection > probe_;
};

// 케이스 1. 정상 경로. 쓰기 3개가 반영되고 슬롯은 유일하게 남음
TEST_F( GameEquipTest, EquippingWritesAllThreeAndLeavesExactlyOneItemInTheSlot )
{
    EquipService service;
    atlas::Ctx ctx;
    ctx.trace_id = 6001;
    ctx.character_id = static_cast< atlas::CharacterId >( kCharacterA );

    ASSERT_FALSE( LastLoginOf( *probe_, kCharacterA ).has_value() );

    atlas::Connection worker( config_ );
    const EquipService::Outcome outcome =
        service.Equip( ctx, worker, RequestFor( kItemSpare, EquipSlot::Weapon ) );

    EXPECT_EQ( outcome.result, EquipResult::Ok );
    EXPECT_EQ( outcome.unequipped_item_uid, kItemWorn );
    EXPECT_EQ( ctx.tx_state, atlas::TxState::Committed );

    // 그 트랜잭션을 본 적 없는 연결에서 읽음
    atlas::Connection reader( config_ );
    EXPECT_EQ( SlotOf( reader, kItemWorn ), static_cast< UInt8 >( EquipSlot::None ) );  // 쓰기 1
    EXPECT_EQ( SlotOf( reader, kItemSpare ),
               static_cast< UInt8 >( EquipSlot::Weapon ) );         // 쓰기 2
    EXPECT_TRUE( LastLoginOf( reader, kCharacterA ).has_value() );  // 쓰기 3
    EXPECT_EQ( CountInSlot( reader, kCharacterA, EquipSlot::Weapon ), std::size_t{ 1 } );
}

// 케이스 2. 세 번째 쓰기에서의 실패가 전부를 되돌린다
// 이 파일 전체의 핵심. 실패 시점에 쓰기 1, 2 는 이미 성공했다
// 트랜잭션이 없으면 슬롯이 빈 채 남는다
// 그 상태는 어떤 DB 제약도 깨지 않아 롤백 말곤 막을 것이 없다
TEST_F( GameEquipTest, AFailureAtTheThirdWriteRollsBackAndLeavesTheInvariantIntact )
{
    EquipService service;
    atlas::Ctx ctx;
    ctx.trace_id = 6002;
    ctx.character_id = static_cast< atlas::CharacterId >( kCharacterA );

    atlas::Connection worker( config_ );
    EXPECT_THROW( service.Equip( ctx, worker, RequestFor( kItemSpare, EquipSlot::Weapon ),
                                 [] { throw std::runtime_error( "the third write failed" ); } ),
                  std::runtime_error );

    // 나가는 길에 ~Transaction 이 롤백해 원장이 Committed 로 남지 않음
    EXPECT_EQ( ctx.tx_state, atlas::TxState::RolledBack );

    atlas::Connection reader( config_ );
    EXPECT_EQ( SlotOf( reader, kItemWorn ), static_cast< UInt8 >( EquipSlot::Weapon ) );
    EXPECT_EQ( SlotOf( reader, kItemSpare ), static_cast< UInt8 >( EquipSlot::None ) );
    EXPECT_FALSE( LastLoginOf( reader, kCharacterA ).has_value() );
    EXPECT_EQ( CountInSlot( reader, kCharacterA, EquipSlot::Weapon ), std::size_t{ 1 } );
}

// 케이스 3. 한 캐릭터에 대한 동시 equip 두 개
// 불변식을 깨는 인터리빙은 두 독자가 같은 점유 상태를 보는 것
// REPEATABLE READ 의 맨 SELECT 는 행 잠금을 안 잡아 InnoDB 가 못 막는다
// 막는 것은 캐릭터별 락이다
// 첫 호출자를 트랜잭션 안에 붙들어 겹침을 사실로 만든다
TEST_F( GameEquipTest, ConcurrentEquipsOnOneCharacterSerialiseAndTheSlotNeverHoldsTwo )
{
    EquipService service;

    std::atomic< bool > first_inside{ false };
    std::atomic< bool > second_entered{ false };
    std::atomic< bool > failed{ false };
    EquipResult first_result = EquipResult::InvalidSlot;
    EquipResult second_result = EquipResult::InvalidSlot;

    // 빠져나가게 두지 않고 잡음. std::thread 를 벗어난 예외는 프로세스를 종료시킴
    std::thread first(
        [&]
        {
            try
            {
                atlas::Connection worker( config_ );
                atlas::Ctx ctx;
                ctx.trace_id = 6003;
                first_result = service
                                   .Equip( ctx, worker, RequestFor( kItemWorn, EquipSlot::Weapon ),
                                           [&]
                                           {
                                               first_inside.store( true );
                                               // 상한 있음. 회귀가 매달리는 대신 실패
                                               WaitUntil( [&] { return second_entered.load(); } );
                                           } )
                                   .result;
            }
            catch ( ... )
            {
                failed.store( true );
            }
            first_inside.store( true );
        } );

    std::thread second(
        [&]
        {
            try
            {
                WaitUntil( [&] { return first_inside.load(); } );
                second_entered.store( true );
                atlas::Connection worker( config_ );
                atlas::Ctx ctx;
                ctx.trace_id = 6004;
                second_result =
                    service.Equip( ctx, worker, RequestFor( kItemSpare, EquipSlot::Weapon ) )
                        .result;
            }
            catch ( ... )
            {
                failed.store( true );
            }
        } );

    first.join();
    second.join();

    ASSERT_FALSE( failed.load() );
    EXPECT_EQ( first_result, EquipResult::Ok );
    EXPECT_EQ( second_result, EquipResult::Ok );

    atlas::Connection reader( config_ );
    // 승자가 아니라 불변식. 최종 상태는 둘 중 어느 쪽이어도 되나 둘 다는 안 됨
    EXPECT_EQ( CountInSlot( reader, kCharacterA, EquipSlot::Weapon ), std::size_t{ 1 } );
    const UInt8 worn = SlotOf( reader, kItemWorn );
    const UInt8 spare = SlotOf( reader, kItemSpare );
    EXPECT_NE( worn == static_cast< UInt8 >( EquipSlot::Weapon ),
               spare == static_cast< UInt8 >( EquipSlot::Weapon ) );
}

// 케이스 4. 존재하지 않는 아이템
TEST_F( GameEquipTest, EquippingAnItemThatDoesNotExistFailsAndWritesNothing )
{
    EquipService service;
    atlas::Ctx ctx;
    ctx.trace_id = 6005;

    atlas::Connection worker( config_ );
    const EquipService::Outcome outcome =
        service.Equip( ctx, worker, RequestFor( kItemMissing, EquipSlot::Weapon ) );

    EXPECT_EQ( outcome.result, EquipResult::ItemNotFound );
    // 트랜잭션이 열린 적 없음. 거부가 쓰기 1 앞이라 되돌릴 것이 없음
    EXPECT_EQ( ctx.tx_state, atlas::TxState::None );

    atlas::Connection reader( config_ );
    EXPECT_EQ( SlotOf( reader, kItemWorn ), static_cast< UInt8 >( EquipSlot::Weapon ) );
    EXPECT_FALSE( LastLoginOf( reader, kCharacterA ).has_value() );
}

// 케이스 5. 다른 캐릭터의 아이템
// [AD 8.2] 3계층. 아이템은 실재하고 요청도 정상. 불법인 것은 묻는 주체
// 체크섬도 HMAC 도 통과하므로 아니라고 말할 수 있는 것은 서버뿐
TEST_F( GameEquipTest, EquippingAnotherCharactersItemIsRefused )
{
    EquipService service;
    atlas::Ctx ctx;
    ctx.trace_id = 6006;

    atlas::Connection worker( config_ );
    const EquipService::Outcome outcome =
        service.Equip( ctx, worker, RequestFor( kItemOfOther, EquipSlot::Weapon ) );

    EXPECT_EQ( outcome.result, EquipResult::NotOwned );
    EXPECT_EQ( ctx.tx_state, atlas::TxState::None );

    atlas::Connection reader( config_ );
    EXPECT_EQ( SlotOf( reader, kItemOfOther ), static_cast< UInt8 >( EquipSlot::None ) );
    EXPECT_EQ( SlotOf( reader, kItemWorn ), static_cast< UInt8 >( EquipSlot::Weapon ) );
    EXPECT_EQ( CountInSlot( reader, kCharacterB, EquipSlot::Weapon ), std::size_t{ 0 } );
}

// 케이스 6. 정의가 존재하지 않는 아이템
// [AD 8.2] 3계층을 정적 데이터에 적용
// 행은 DB 에 있고 요청자 소유라 이전 검사는 다 통과
// 틀린 것은 item.csv 가 정의하지 않은 item_id
TEST_F( GameEquipTest, EquippingAnItemWithNoDefinitionIsRefusedAndWritesNothing )
{
    EquipService service;
    atlas::Ctx ctx;
    ctx.trace_id = 6007;

    atlas::Connection worker( config_ );
    const EquipService::Outcome outcome =
        service.Equip( ctx, worker, RequestFor( kItemUndefined, EquipSlot::Weapon ) );

    EXPECT_EQ( outcome.result, EquipResult::UnknownItem );
    // 거부가 트랜잭션 앞에 서 있어 되돌릴 것이 없음
    EXPECT_EQ( ctx.tx_state, atlas::TxState::None );

    atlas::Connection reader( config_ );
    EXPECT_EQ( SlotOf( reader, kItemUndefined ), static_cast< UInt8 >( EquipSlot::None ) );
    EXPECT_EQ( SlotOf( reader, kItemWorn ), static_cast< UInt8 >( EquipSlot::Weapon ) );
    EXPECT_FALSE( LastLoginOf( reader, kCharacterA ).has_value() );
}

// 케이스 7. 맞는 아이템, 틀린 슬롯
// 정적 데이터가 생겨서 비로소 존재하는 케이스
// equip_slot 은 패킷이 실어 온 바이트였고 반박할 근거가 없었다
// item_id -> 허용 슬롯이 그 첫 수단
TEST_F( GameEquipTest, EquippingAnItemIntoASlotItsDefinitionForbidsIsRefused )
{
    EquipService service;
    atlas::Ctx ctx;
    ctx.trace_id = 6008;

    atlas::Connection worker( config_ );
    // kItemCharm 은 item.csv 에서 slot 3(장신구). 요청은 무기 슬롯을 지목
    const EquipService::Outcome outcome =
        service.Equip( ctx, worker, RequestFor( kItemCharm, EquipSlot::Weapon ) );

    EXPECT_EQ( outcome.result, EquipResult::SlotMismatch );
    EXPECT_EQ( ctx.tx_state, atlas::TxState::None );

    atlas::Connection reader( config_ );
    EXPECT_EQ( SlotOf( reader, kItemCharm ), static_cast< UInt8 >( EquipSlot::None ) );
    // 요청이 겨눈 슬롯의 점유자도 그대로. 쓰기 1 을 돌린 거부였다면 비었을 것
    EXPECT_EQ( SlotOf( reader, kItemWorn ), static_cast< UInt8 >( EquipSlot::Weapon ) );
    EXPECT_EQ( CountInSlot( reader, kCharacterA, EquipSlot::Weapon ), std::size_t{ 1 } );

    // 정의가 지목한 슬롯으로는 성공. 위 거부는 슬롯 문제였지 아이템 문제가 아님
    atlas::Ctx allowed_ctx;
    allowed_ctx.trace_id = 6009;
    EXPECT_EQ(
        service.Equip( allowed_ctx, worker, RequestFor( kItemCharm, EquipSlot::Trinket ) ).result,
        EquipResult::Ok );
    EXPECT_EQ( SlotOf( reader, kItemCharm ), static_cast< UInt8 >( EquipSlot::Trinket ) );
}

// =============================================================================
// 종단 - 실제 소켓 위에서
// =============================================================================

// [AD 8.1] client/ 가 아직 없으므로 클라이언트는 이 테스트 자신
// 실제 TCP 소켓이 실제 프레임 형식으로 실제 스택을 두드린다
// 우회하면 증명이 사라진다

// 테스트 스레드가 직접 모는 동기 클라이언트. 단언이 위에서 아래로 읽힘
class TestClient
{
public:
    explicit TestClient( const atlas::Endpoint& endpoint ) : socket_( io_context_ )
    {
        socket_.connect( endpoint );
    }

    TestClient( const TestClient& ) = delete;
    TestClient& operator=( const TestClient& ) = delete;

    ~TestClient()
    {
        atlas::ErrorCode ignored;
        std::ignore = socket_.shutdown( atlas::Socket::shutdown_both, ignored );
        std::ignore = socket_.close( ignored );
    }

    // [AD 8.3] 클라이언트도 자기 송신 카운터를 가짐. 중복이나 역행은 연결 종료
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
        // [AD 8.2] 1계층. 신뢰하지 않고 검증. 건너뛰면 그 계층을 안 쓰는 셈
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
    UInt64 account_uid{ 0 };
    std::string name{};
    UInt16 level{ 0 };
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

    atlas::Int32 pos_x = 0;
    atlas::Int32 pos_y = 0;
    UInt64 exp = 0;
    UInt16 count = 0;
    EXPECT_TRUE( atlas::generated::ReadLe( cursor, parsed.character_id ) );
    EXPECT_TRUE( atlas::generated::ReadLe( cursor, parsed.account_uid ) );
    EXPECT_TRUE( atlas::generated::ReadUtf8( cursor, parsed.name ) );
    EXPECT_TRUE( atlas::generated::ReadLe( cursor, pos_x ) );
    EXPECT_TRUE( atlas::generated::ReadLe( cursor, pos_y ) );
    EXPECT_TRUE( atlas::generated::ReadLe( cursor, parsed.level ) );
    EXPECT_TRUE( atlas::generated::ReadLe( cursor, exp ) );
    EXPECT_TRUE( atlas::generated::ReadLe( cursor, count ) );
    for ( UInt16 index = 0; index < count; ++index )
    {
        atlas_demo::Item item;
        UInt8 slot = 0;
        EXPECT_TRUE( atlas::generated::ReadLe( cursor, item.item_uid ) );
        EXPECT_TRUE( atlas::generated::ReadLe( cursor, item.item_id ) );
        EXPECT_TRUE( atlas::generated::ReadLe( cursor, item.stack_count ) );
        EXPECT_TRUE( atlas::generated::ReadLe( cursor, slot ) );
        item.slot = static_cast< EquipSlot >( slot );
        parsed.items.push_back( item );
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
    for ( const atlas_demo::Item& item : loaded.items )
    {
        if ( item.item_uid == item_uid )
        {
            return &item;
        }
    }
    return nullptr;
}

TEST_F( GameEquipTest, AClientLoadsEquipsReconnectsAndTheEquipmentIsStillThere )
{
    atlas_demo::GameServer::Options options;
    // any 주소가 아닌 loopback, 포트 0. 병렬 ctest 끼리 충돌 방지
    options.endpoint = atlas::Endpoint( atlas::asio::ip::address_v4::loopback(), 0 );
    options.server_id = kServerId;
    options.io_workers = 2;
    options.db_pool_size = 2;
    options.db_threads = 2;

    atlas_demo::GameServer server( options, config_ );
    server.Start();

    {
        TestClient client( server.LocalEndpoint() );

        client.Send( atlas_demo::kOpCharacterLoadRequest, LoadRequestPayload( kCharacterA ) );
        const atlas::Frame load_response = client.Receive();
        ASSERT_EQ( load_response.opcode, atlas_demo::kOpCharacterLoadResponse );

        const LoadedCharacter loaded = ParseLoadResponse( load_response );
        ASSERT_EQ( loaded.result, static_cast< UInt8 >( atlas_demo::LoadResult::Ok ) );
        EXPECT_EQ( loaded.character_id, kCharacterA );
        EXPECT_EQ( loaded.account_uid, kAccountUid );
        EXPECT_EQ( loaded.name, "atlas-a" );
        ASSERT_EQ( loaded.items.size(), std::size_t{ 4 } );
        ASSERT_NE( FindItem( loaded, kItemCharm ), nullptr );
        EXPECT_EQ( FindItem( loaded, kItemCharm )->slot, EquipSlot::None );

        client.Send( atlas_demo::kOpEquipRequest,
                     EquipRequestPayload( kItemCharm, EquipSlot::Trinket ) );
        const atlas::Frame equip_response = client.Receive();
        ASSERT_EQ( equip_response.opcode, atlas_demo::kOpEquipResponse );

        std::span< const Byte > cursor( equip_response.payload );
        UInt8 code = 0xFF;
        UInt64 echoed_uid = 0;
        UInt8 echoed_slot = 0;
        UInt64 unequipped = 0;
        ASSERT_TRUE( atlas::generated::ReadLe( cursor, code ) );
        ASSERT_TRUE( atlas::generated::ReadLe( cursor, echoed_uid ) );
        ASSERT_TRUE( atlas::generated::ReadLe( cursor, echoed_slot ) );
        ASSERT_TRUE( atlas::generated::ReadLe( cursor, unequipped ) );
        EXPECT_EQ( code, static_cast< UInt8 >( EquipResult::Ok ) );
        EXPECT_EQ( echoed_uid, kItemCharm );
        EXPECT_EQ( echoed_slot, static_cast< UInt8 >( EquipSlot::Trinket ) );
        // 장신구 슬롯이 비어 있어 벗겨진 것 없음
        EXPECT_EQ( unequipped, UInt64{ 0 } );

        // 살아 있는 연결 하나 위의 거부 3회. 연결 자체가 단언
        // [AD 8.2] 길이, seq, 체크섬이 정상이라 1-2계층은 이의 없다
        // 거부는 3계층만의 것. 답은 close 가 아니라 실패 응답
        // 다음 요청이 답을 받는 것이 그 증거
        const auto refuse = [&]( UInt64 item_uid, EquipSlot slot )
        {
            client.Send( atlas_demo::kOpEquipRequest, EquipRequestPayload( item_uid, slot ) );
            const atlas::Frame refused = client.Receive();
            EXPECT_EQ( refused.opcode, atlas_demo::kOpEquipResponse );
            std::span< const Byte > refusal( refused.payload );
            UInt8 refusal_code = 0;
            EXPECT_TRUE( atlas::generated::ReadLe( refusal, refusal_code ) );
            return refusal_code;
        };

        // 캐릭터 A 가 B 의 아이템을 요구
        EXPECT_EQ( refuse( kItemOfOther, EquipSlot::Weapon ),
                   static_cast< UInt8 >( EquipResult::NotOwned ) );
        // A 소유이지만 item.csv 가 그 item_id 를 정의하지 않은 행
        EXPECT_EQ( refuse( kItemUndefined, EquipSlot::Weapon ),
                   static_cast< UInt8 >( EquipResult::UnknownItem ) );
        // A 자신의 장신구를 무기라 주장
        EXPECT_EQ( refuse( kItemCharm, EquipSlot::Weapon ),
                   static_cast< UInt8 >( EquipResult::SlotMismatch ) );

        // 여전히 대화 중이고 여전히 정확. 거부 3회를 받고도 신원을 지킴
        client.Send( atlas_demo::kOpCharacterLoadRequest, LoadRequestPayload( kCharacterA ) );
        const LoadedCharacter after_refusals = ParseLoadResponse( client.Receive() );
        ASSERT_EQ( after_refusals.result, static_cast< UInt8 >( atlas_demo::LoadResult::Ok ) );
        EXPECT_EQ( after_refusals.character_id, kCharacterA );
    }  // 여기서 클라이언트가 끊음

    ASSERT_TRUE( WaitUntil( [&] { return server.LiveSessionCount() == 0; } ) );

    // 재접속. 새 연결, 새 세션. 장비는 앞 연결이 남긴 메모리가 아니라 DB 에서 읽힘
    {
        TestClient reconnected( server.LocalEndpoint() );
        reconnected.Send( atlas_demo::kOpCharacterLoadRequest, LoadRequestPayload( kCharacterA ) );
        const LoadedCharacter loaded = ParseLoadResponse( reconnected.Receive() );

        ASSERT_EQ( loaded.result, static_cast< UInt8 >( atlas_demo::LoadResult::Ok ) );
        ASSERT_NE( FindItem( loaded, kItemCharm ), nullptr );
        EXPECT_EQ( FindItem( loaded, kItemCharm )->slot, EquipSlot::Trinket );
        // 무기 슬롯은 그대로. 거부된 요청 3개는 아무것도 쓰지 않음
        ASSERT_NE( FindItem( loaded, kItemWorn ), nullptr );
        EXPECT_EQ( FindItem( loaded, kItemWorn )->slot, EquipSlot::Weapon );
    }

    // [AD 9] 현관문, 세션, 그다음 풀. 직접 부르는 것이 그 순서를 시험대에 올림
    server.Stop();
    EXPECT_EQ( server.LiveSessionCount(), std::size_t{ 0 } );
}

// =============================================================================
// 도메인 모델 - DB 불필요, 항상 실행
// =============================================================================

// 이 파일이 못 박으려는 구분
// 다음 독자가 Account 와 Character 를 상속으로 "단순화" 하면 컴파일되지 않는다
TEST( DomainOwnershipTest, AnAccountOwnsManyCharactersAndIsNotABaseClassOfThem )
{
    static_assert( !std::is_base_of_v< atlas_demo::Account, atlas_demo::Character >,
                   "Account -> Character is OWNERSHIP (1:N), never inheritance: an is-a chain "
                   "cannot express a second character of the same account" );

    atlas_demo::Account account( static_cast< atlas::AccountId >( kAccountUid ) );
    const atlas_demo::Character first( kServerId, static_cast< atlas::CharacterId >( kCharacterA ),
                                       static_cast< atlas::AccountId >( kAccountUid ), "atlas-a", 0,
                                       0, 1, 0 );
    const atlas_demo::Character second( kServerId, static_cast< atlas::CharacterId >( kCharacterB ),
                                        static_cast< atlas::AccountId >( kAccountUid ), "atlas-b",
                                        0, 0, 1, 0 );

    account.Own( first.CharacterKey() );
    account.Own( second.CharacterKey() );
    // 멱등. 같은 캐릭터를 두 번 소유해도 캐릭터가 둘이 되지는 않음
    account.Own( second.CharacterKey() );

    EXPECT_EQ( account.Characters().size(), std::size_t{ 2 } );
    EXPECT_TRUE( account.Owns( first.CharacterKey() ) );
    EXPECT_TRUE( account.Owns( second.CharacterKey() ) );
    EXPECT_FALSE( account.Owns( static_cast< atlas::CharacterId >( kItemMissing ) ) );
    EXPECT_EQ( first.AccountUid(), second.AccountUid() );
}

// 다른 축. Entity -> Character / Npc 는 진짜 is-a 이고, 파생이 둘이라 디스패치도 실재
TEST( DomainInheritanceTest, EntityDispatchesToCharacterAndNpc )
{
    const atlas_demo::Character character(
        kServerId, static_cast< atlas::CharacterId >( kCharacterA ),
        static_cast< atlas::AccountId >( kAccountUid ), "atlas-a", 3, 4, 7, 120 );
    const atlas_demo::Npc npc( static_cast< atlas::ActorId >( 42 ), 900, -3, -4, 2,
                               "training dummy" );

    const std::array< const atlas_demo::Entity*, 2 > world{ &character, &npc };
    EXPECT_EQ( world[0]->Kind(), atlas_demo::EntityKind::Character );
    EXPECT_EQ( world[1]->Kind(), atlas_demo::EntityKind::Npc );
    EXPECT_EQ( world[0]->DisplayName(), "atlas-a" );
    EXPECT_EQ( world[1]->DisplayName(), "training dummy" );
    EXPECT_EQ( world[0]->Level(), UInt16{ 7 } );
    EXPECT_EQ( world[1]->PosX(), atlas::Int32{ -3 } );
    // 월드 신원은 지속 신원에서 유도. Character 생성자 참고
    EXPECT_EQ( atlas::IdValue( world[0]->Id() ), kCharacterA );
}

TEST( DomainInventoryTest, EquippingClearsTheSlotItMovesInto )
{
    atlas_demo::Inventory inventory;
    inventory.Add( atlas_demo::Item{
        .item_uid = kItemWorn, .item_id = 11, .stack_count = 1, .slot = EquipSlot::Weapon } );
    inventory.Add( atlas_demo::Item{
        .item_uid = kItemSpare, .item_id = 12, .stack_count = 1, .slot = EquipSlot::None } );

    ASSERT_TRUE( inventory.Equip( kItemSpare, EquipSlot::Weapon ) );

    ASSERT_NE( inventory.EquippedAt( EquipSlot::Weapon ), nullptr );
    EXPECT_EQ( inventory.EquippedAt( EquipSlot::Weapon )->item_uid, kItemSpare );
    ASSERT_NE( inventory.Find( kItemWorn ), nullptr );
    EXPECT_FALSE( inventory.Find( kItemWorn )->Equipped() );
}

TEST( DomainInventoryTest, SlotZeroIsNotAnEquipTargetAndAnUnheldItemIsRefused )
{
    atlas_demo::Inventory inventory;
    inventory.Add( atlas_demo::Item{
        .item_uid = kItemWorn, .item_id = 11, .stack_count = 1, .slot = EquipSlot::None } );

    // "슬롯 0 으로 equip" 은 옷 갈아입은 unequip. 통과시키면 로그에서 구분 불가
    EXPECT_FALSE( inventory.Equip( kItemWorn, EquipSlot::None ) );
    EXPECT_FALSE( inventory.Equip( kItemMissing, EquipSlot::Weapon ) );
    // [AD 8.2] 범위 밖 캐스트가 이 단언의 요점
    // 어떤 열거자도 이름하지 않은 값을 IsEquippableSlot 이 거부해야 한다
    // 변조된 패킷이 실어 오는 것이 정확히 그것
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    EXPECT_FALSE( IsEquippableSlot( static_cast< EquipSlot >( atlas_demo::kEquipSlotCount + 1 ) ) );
    EXPECT_TRUE( IsEquippableSlot( EquipSlot::Trinket ) );
}

}  // namespace
