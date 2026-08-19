// =============================================================================
// 손으로 쓴 db_generator 게이트. 생성물이 아니라 생성물을 검사하는 쪽
// [AD 10] SQL 을 실행하지 않음. 커넥션 - 트랜잭션은 ORM 런타임의 몫
// =============================================================================

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string_view>
#include <type_traits>

#include "atlas/core/ids.h"
#include "atlas/core/types.h"
#include "generated/db/characters_row.h"
#include "generated/db/db_meta.h"

namespace
{

using atlas::AccountId;
using atlas::CharacterId;
using atlas::IdValue;
using atlas::Int32;
using atlas::UInt16;
using atlas::UInt64;
using atlas::generated::ColumnMeta;
using atlas::generated::ColumnType;
using atlas::generated::CountPlaceholders;

// =============================================================================
// 1. 스키마 대 구조체
// =============================================================================

// schema.json 에서 손으로 옮겨 적은 기대값
// 재생성하지 말 것. 실패를 없애려고 고치지도 말 것
// 같은 입력에서 낸 테스트는 생성기가 자기와 일치함만 증명한다
constexpr std::array< ColumnMeta, 10 > kExpectedCharacterColumns = { {
    { .name_ = "server_id",
      .type_ = ColumnType::UInt16,
      .primary_key_ = true,
      .nullable_ = false,
      .max_length_ = 0 },
    { .name_ = "character_id",
      .type_ = ColumnType::UInt64,
      .primary_key_ = true,
      .nullable_ = false,
      .max_length_ = 0 },
    { .name_ = "account_uid",
      .type_ = ColumnType::UInt64,
      .primary_key_ = false,
      .nullable_ = false,
      .max_length_ = 0 },
    { .name_ = "name",
      .type_ = ColumnType::String,
      .primary_key_ = false,
      .nullable_ = false,
      .max_length_ = 32 },
    { .name_ = "pos_x",
      .type_ = ColumnType::Int32,
      .primary_key_ = false,
      .nullable_ = false,
      .max_length_ = 0 },
    { .name_ = "pos_y",
      .type_ = ColumnType::Int32,
      .primary_key_ = false,
      .nullable_ = false,
      .max_length_ = 0 },
    { .name_ = "level",
      .type_ = ColumnType::UInt16,
      .primary_key_ = false,
      .nullable_ = false,
      .max_length_ = 0 },
    { .name_ = "exp",
      .type_ = ColumnType::UInt64,
      .primary_key_ = false,
      .nullable_ = false,
      .max_length_ = 0 },
    { .name_ = "created_at",
      .type_ = ColumnType::DateTime,
      .primary_key_ = false,
      .nullable_ = false,
      .max_length_ = 0 },
    { .name_ = "last_login_at",
      .type_ = ColumnType::DateTime,
      .primary_key_ = false,
      .nullable_ = true,
      .max_length_ = 0 },
} };

TEST( DbSchema, MetadataMatchesTheSchemaColumnByColumn )
{
    ASSERT_EQ( atlas::generated::kCharactersColumnCount, kExpectedCharacterColumns.size() );
    ASSERT_EQ( atlas::generated::kCharactersColumns.size(), kExpectedCharacterColumns.size() );
    EXPECT_EQ( atlas::generated::kCharactersTable, "characters" );

    for ( std::size_t i = 0; i < kExpectedCharacterColumns.size(); ++i )
    {
        const ColumnMeta& want = kExpectedCharacterColumns[i];
        const ColumnMeta& got = atlas::generated::kCharactersColumns[i];
        EXPECT_EQ( got.name_, want.name_ ) << "column " << i;
        EXPECT_EQ( got.type_, want.type_ ) << "column " << want.name_;
        EXPECT_EQ( got.primary_key_, want.primary_key_ ) << "column " << want.name_;
        EXPECT_EQ( got.nullable_, want.nullable_ ) << "column " << want.name_;
        EXPECT_EQ( got.max_length_, want.max_length_ ) << "column " << want.name_;
    }
}

TEST( DbSchema, PrimaryKeyIsTheServerScopedCharacterPair )
{
    // [AD 6] 계정 하나가 여러 서버에 캐릭터 보유. 키는 쌍이며 단독 불가
    std::size_t key_columns = 0;
    for ( const ColumnMeta& column : atlas::generated::kCharactersColumns )
    {
        if ( column.primary_key_ ) ++key_columns;
    }
    EXPECT_EQ( key_columns, 2U );
    EXPECT_TRUE( atlas::generated::kCharactersColumns[atlas::generated::kCharactersColServerId]
                     .primary_key_ );
    EXPECT_TRUE( atlas::generated::kCharactersColumns[atlas::generated::kCharactersColCharacterId]
                     .primary_key_ );
}

TEST( DbSchema, RowFieldsCoverEveryColumn )
{
    // 모든 필드를 이름으로 지정해 구조체와 스키마를 컴파일 시점에 결합
    // 컬럼이 바뀌면 빌드가 깨짐. 실패 단언보다 큰 소리
    const atlas::generated::CharactersRow row{
        .server_id_ = UInt16{ 7 },
        .character_id_ = CharacterId{ 42 },
        .account_uid_ = AccountId{ 1234567890123456U },
        .name_ = "atlas",
        .pos_x_ = Int32{ -3 },
        .pos_y_ = Int32{ 9 },
        .level_ = UInt16{ 5 },
        .exp_ = UInt64{ 999 },
        .created_at_ = {},
        .last_login_at_ = {},
    };

    EXPECT_EQ( IdValue( row.character_id_ ), 42U );
    EXPECT_EQ( row.name_.size(), 5U );
    EXPECT_LE( row.name_.size(), atlas::generated::kCharactersNameMaxLength );
    EXPECT_FALSE( row.last_login_at_.has_value() )
        << "a nullable column defaults to absent, not to 0";

    // 기본값도 스키마에서 옴. 둘이 어긋날 수 없음
    const atlas::generated::CharactersRow fresh{};
    EXPECT_EQ( fresh.level_, 1U );
    EXPECT_EQ( fresh.exp_, 0U );
    EXPECT_EQ( fresh.pos_x_, 0 );
    EXPECT_EQ( fresh.pos_y_, 0 );
}

// =============================================================================
// 2. 고정 폭
// =============================================================================

// [CS 4.1] Windows LLP64, Linux LP64. 크기 변동의 근거를 테스트 옆에 둠
static_assert( sizeof( atlas::generated::CharactersRow::character_id_ ) == 8U,
               "characters.character_id is a 64-bit identifier on every platform" );
static_assert( sizeof( atlas::generated::CharactersRow::exp_ ) == 8U,
               "characters.exp is a 64-bit counter on every platform" );
static_assert( sizeof( atlas::generated::CharactersRow::pos_x_ ) == 4U,
               "characters.pos_x is a 32-bit grid coordinate on every platform" );
static_assert( sizeof( atlas::generated::CharactersRow::server_id_ ) == 2U,
               "characters.server_id is a 16-bit scope on every platform" );

// =============================================================================
// 3. prepared statement
// =============================================================================

TEST( DbSchema, EveryStatementBindsExactlyItsPlaceholders )
{
    EXPECT_EQ( CountPlaceholders( atlas::generated::kCharactersSelectByPkSql ),
               atlas::generated::kCharactersSelectByPkBinding.size() );
    EXPECT_EQ( CountPlaceholders( atlas::generated::kCharactersInsertSql ),
               atlas::generated::kCharactersInsertBinding.size() );
    EXPECT_EQ( CountPlaceholders( atlas::generated::kCharactersUpdateByPkSql ),
               atlas::generated::kCharactersUpdateByPkBinding.size() );
    EXPECT_EQ( CountPlaceholders( atlas::generated::kCharactersDeleteByPkSql ),
               atlas::generated::kCharactersDeleteByPkBinding.size() );
}

TEST( DbSchema, BindingIndicesStayInsideTheColumnTable )
{
    const auto in_range = []( std::size_t index )
    { return index < atlas::generated::kCharactersColumnCount; };
    for ( const std::size_t index : atlas::generated::kCharactersInsertBinding )
        EXPECT_TRUE( in_range( index ) );
    for ( const std::size_t index : atlas::generated::kCharactersUpdateByPkBinding )
        EXPECT_TRUE( in_range( index ) );
    for ( const std::size_t index : atlas::generated::kCharactersSelectByPkBinding )
        EXPECT_TRUE( in_range( index ) );
    for ( const std::size_t index : atlas::generated::kCharactersDeleteByPkBinding )
        EXPECT_TRUE( in_range( index ) );
}

TEST( DbSchema, UpdateBindsTheKeyColumnsLast )
{
    // WHERE 는 SET 뒤이므로 키 파라미터가 마지막 둘
    // 앞에 바인딩하면 문장은 멀쩡해 보인 채 엉뚱한 행이 갱신됨
    const auto& binding = atlas::generated::kCharactersUpdateByPkBinding;
    ASSERT_EQ( binding.size(), atlas::generated::kCharactersColumnCount );
    EXPECT_EQ( binding[binding.size() - 2], atlas::generated::kCharactersColServerId );
    EXPECT_EQ( binding[binding.size() - 1], atlas::generated::kCharactersColCharacterId );
}

TEST( DbSchema, StatementsAreFixedTextWithPlaceholders )
{
    // [AD 10] SQL 조립 없음. 값은 플레이스홀더로만 DB 에 도달
    // 따라서 문장에는 ? 가 있고 인용 리터럴이 없어야 함
    const std::array< std::string_view, 4 > statements = { {
        atlas::generated::kCharactersSelectByPkSql,
        atlas::generated::kCharactersInsertSql,
        atlas::generated::kCharactersUpdateByPkSql,
        atlas::generated::kCharactersDeleteByPkSql,
    } };
    for ( const std::string_view sql : statements )
    {
        EXPECT_GT( CountPlaceholders( sql ), 0U ) << sql;
        EXPECT_EQ( sql.find( '\'' ), std::string_view::npos ) << "quoted literal in " << sql;
    }
}

TEST( DbSchema, SelectProjectsEveryColumnInMetadataOrder )
{
    // 행 구조체를 결과셋에서 위치로 채우므로 투영 순서 = 메타데이터 순서
    const std::string_view sql = atlas::generated::kCharactersSelectByPkSql;
    std::size_t cursor = 0;
    for ( const ColumnMeta& column : atlas::generated::kCharactersColumns )
    {
        const std::size_t found = sql.find( column.name_, cursor );
        ASSERT_NE( found, std::string_view::npos )
            << "column missing from projection: " << column.name_;
        cursor = found + column.name_.size();
    }
}

// =============================================================================
// 4. 강타입 ID
// =============================================================================

// [CS 4.3] 규칙의 값어치는 존재하지 않는 변환에 있다
// 하나라도 뒤집히면 인자가 뒤바뀐 Load 호출이 그대로 컴파일된다
static_assert(
    std::is_same_v< decltype( atlas::generated::CharactersRow::character_id_ ), CharacterId >,
    "characters.character_id must be the strong-typed CharacterId" );
static_assert(
    std::is_same_v< decltype( atlas::generated::CharactersRow::account_uid_ ), AccountId >,
    "characters.account_uid must be the strong-typed AccountId" );
static_assert( !std::is_convertible_v< CharacterId, UInt64 >,
               "CharacterId must not decay to its underlying value implicitly" );
static_assert( !std::is_convertible_v< UInt64, CharacterId >,
               "a raw integer must not become a CharacterId implicitly" );
static_assert( !std::is_convertible_v< CharacterId, AccountId >,
               "two identifiers of the same width must stay distinct types" );
static_assert(
    !std::is_same_v< decltype( atlas::generated::CharactersRow::server_id_ ), CharacterId >,
    "server_id is deliberately still a plain width (see atlas/core/ids.h)" );

TEST( DbSchema, IdentifierValueSurvivesTheStrongType )
{
    // 원시 값으로 돌아가는 유일한 명시 경로. 컬럼 자체는 UInt64 저장
    const CharacterId id{ 0xFFFF'FFFF'FFFF'FFFFU };
    EXPECT_EQ( IdValue( id ), 0xFFFF'FFFF'FFFF'FFFFU );
    EXPECT_EQ(
        atlas::generated::kCharactersColumns[atlas::generated::kCharactersColCharacterId].type_,
        ColumnType::UInt64 );
}

}  // namespace
