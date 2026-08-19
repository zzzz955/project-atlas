// =============================================================================
// [AD 14] info_generator 가 낸 정적 데이터 테이블 검사
// 기대값은 shared/datas/item.csv 에서 손으로 옮겨 적음
// =============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>

#include "atlas/core/types.h"
// 일부러 집합 헤더. 이렇게 포함하는 곳이 있어야 "include 하나" 가 참이 됨
#include "generated/info/info_all.h"

// [AD 8.2] 이 스위트가 주장하는 것
// - PK 조회는 CSV 가 선언한 행을 돌려준다
// - 없는 id 는 이웃 행도 예외도 아닌 nullptr. equip 경로가 이 null 에 의존
// - 행이 키 오름차순으로 나온다. 조회가 스캔 아닌 이진 탐색인 근거
// - C 컬럼은 서버 struct 에 없다. 아무도 안 보는 필터는 조용히 멈춘다

namespace
{

using atlas::UInt32;
using atlas::UInt8;
using atlas::generated::FindItemInfo;
using atlas::generated::ItemInfoRow;
using atlas::generated::ItemInfoRows;

// shared/datas/item.csv. slot: 1 = weapon, 2 = armor, 3 = trinket
struct ExpectedItem
{
    UInt32 id;
    UInt8 slot;
};

constexpr std::array< ExpectedItem, 4 > kExpected = { {
    { .id = 1001, .slot = 1 },
    { .id = 1002, .slot = 1 },
    { .id = 2001, .slot = 2 },
    { .id = 3001, .slot = 3 },
} };

TEST( InfoTableTest, EveryRowOfTheCsvIsPresentAndFoundByItsKey )
{
    ASSERT_EQ( ItemInfoRows().size(), kExpected.size() );
    ASSERT_EQ( atlas::generated::kItemInfoRowCount, kExpected.size() );

    for ( const ExpectedItem& expected : kExpected )
    {
        const ItemInfoRow* found = FindItemInfo( expected.id );
        ASSERT_NE( found, nullptr ) << "item_id " << expected.id << " is missing";
        EXPECT_EQ( found->id_, expected.id );
        EXPECT_EQ( found->slot_, expected.slot );
    }
}

// 정의되지 않은 id 는 거부로 이어져야 함. 이웃 행이나 throw 로는 거부 불가
TEST( InfoTableTest, AnIdTheCsvDoesNotDeclareIsNullAndNotANeighbour )
{
    EXPECT_EQ( FindItemInfo( 0 ), nullptr );
    EXPECT_EQ( FindItemInfo( 1000 ), nullptr );  // 첫 키 바로 아래
    EXPECT_EQ( FindItemInfo( 1500 ), nullptr );  // 두 키 사이
    EXPECT_EQ( FindItemInfo( 9999 ), nullptr );  // 마지막 키 너머
}

// 이진 탐색이라 정렬은 장식이 아님. 안 맞으면 있는 행도 nullptr 이 됨
TEST( InfoTableTest, RowsAreEmittedInAscendingKeyOrder )
{
    const std::span< const ItemInfoRow > rows = ItemInfoRows();
    EXPECT_TRUE( std::ranges::is_sorted(
        rows, []( const ItemInfoRow& a, const ItemInfoRow& b ) { return a.id_ < b.id_; } ) );
}

// 봉합면 검증. 클라 전용 컬럼이 새어 들어오면 이 파일이 컴파일되지 않음
TEST( InfoTableTest, ClientOnlyColumnsAreNotInTheServerStruct )
{
    EXPECT_EQ( atlas::generated::kItemInfoColumnCount, std::size_t{ 2 } );
    ASSERT_EQ( atlas::generated::kItemInfoColumns.size(), std::size_t{ 2 } );
    EXPECT_EQ( atlas::generated::kItemInfoColumns[0].name_, "id" );
    EXPECT_TRUE( atlas::generated::kItemInfoColumns[0].primary_key_ );
    EXPECT_EQ( atlas::generated::kItemInfoColumns[1].name_, "slot" );

    for ( const atlas::generated::InfoColumnMeta& column : atlas::generated::kItemInfoColumns )
    {
        EXPECT_NE( column.target_, "C" ) << column.name_ << " is client-only and must not be here";
    }

    // [CS 4.1] int 가 아닌 고정폭. types.json 에 int 키 자체가 없음
    static_assert( std::is_same_v< decltype( ItemInfoRow::id_ ), UInt32 > );
    static_assert( std::is_same_v< decltype( ItemInfoRow::slot_ ), UInt8 > );
}

}  // namespace
