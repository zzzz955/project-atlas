// =============================================================================
// 코어 타입 규약 검사. 별칭 폭, 부호, 강타입 ID, 시계 분리
// =============================================================================

#include <gtest/gtest.h>

#include <cstddef>
#include <type_traits>

#include "atlas/core/time.h"
#include "atlas/core/types.h"

namespace
{

// [CS 4.3] 강타입 ID 패턴. 코어가 아직 ID 를 갖지 않아 여기에 선언
enum class ActorId : atlas::UInt64
{
};

TEST( CoreTypes, FixedWidthAliasesHaveTheDeclaredWidth )
{
    EXPECT_EQ( sizeof( atlas::Int8 ), std::size_t{ 1 } );
    EXPECT_EQ( sizeof( atlas::UInt8 ), std::size_t{ 1 } );
    EXPECT_EQ( sizeof( atlas::Int16 ), std::size_t{ 2 } );
    EXPECT_EQ( sizeof( atlas::UInt16 ), std::size_t{ 2 } );
    EXPECT_EQ( sizeof( atlas::Int32 ), std::size_t{ 4 } );
    EXPECT_EQ( sizeof( atlas::UInt32 ), std::size_t{ 4 } );
    EXPECT_EQ( sizeof( atlas::Int64 ), std::size_t{ 8 } );
    EXPECT_EQ( sizeof( atlas::UInt64 ), std::size_t{ 8 } );
    EXPECT_EQ( sizeof( atlas::Float32 ), std::size_t{ 4 } );
    EXPECT_EQ( sizeof( atlas::Float64 ), std::size_t{ 8 } );
    EXPECT_EQ( sizeof( atlas::Byte ), std::size_t{ 1 } );
}

TEST( CoreTypes, SignednessMatchesTheAliasName )
{
    EXPECT_TRUE( std::is_signed_v< atlas::Int32 > );
    EXPECT_TRUE( std::is_signed_v< atlas::Int64 > );
    EXPECT_FALSE( std::is_signed_v< atlas::UInt32 > );
    EXPECT_FALSE( std::is_signed_v< atlas::UInt64 > );
}

TEST( StrongTypedId, DoesNotConvertImplicitlyToItsUnderlyingType )
{
    // ID 인자 뒤바꿈이 조용한 런타임 버그가 아니라 컴파일 에러가 될 것
    static_assert( !std::is_convertible_v< ActorId, atlas::UInt64 > );
    static_assert( !std::is_convertible_v< atlas::UInt64, ActorId > );
    EXPECT_EQ( static_cast< atlas::UInt64 >( ActorId{ 7 } ), atlas::UInt64{ 7 } );
}

TEST( CoreTime, SteadyAndWallClocksAreDistinctTypes )
{
    // [CS 4.2] 섞어 쓰면 NTP 보정이 벽시계를 되돌릴 때 타임아웃이 무한 대기가 됨
    static_assert( !std::is_same_v< atlas::Clock, atlas::SysClock > );
    static_assert( atlas::Clock::is_steady );
    EXPECT_GT( atlas::Millis{ 100 }, atlas::Millis{ 1 } );
}

}  // namespace
