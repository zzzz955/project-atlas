// =============================================================================
// [AD 8.4] 손으로 쓴 pkt_generator 게이트. 왕복 - 절단 거부 - 바이트 순서
// =============================================================================

#include "generated/pkt/pkt_codec.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "atlas/core/types.h"
#include "generated/pkt/chat_broadcast.h"
#include "generated/pkt/contract_enums.h"
#include "generated/pkt/move_input_request.h"
#include "generated/pkt/move_state_broadcast.h"

namespace
{

using atlas::Byte;
using atlas::Int16;
using atlas::UInt16;
using atlas::UInt32;
using atlas::UInt64;
using atlas::UInt8;
using atlas::generated::ChatBroadcast;
using atlas::generated::ChatChannel;
using atlas::generated::MoveDirection;
using atlas::generated::MoveInputRequest;
using atlas::generated::MoveStateBroadcast;

std::vector< Byte > ToBytes( const std::vector< UInt8 >& values )
{
    std::vector< Byte > bytes;
    bytes.reserve( values.size() );
    for ( const UInt8 value : values ) bytes.push_back( static_cast< Byte >( value ) );
    return bytes;
}

std::vector< UInt8 > FromBytes( const std::vector< Byte >& bytes )
{
    std::vector< UInt8 > values;
    values.reserve( bytes.size() );
    for ( const Byte byte : bytes ) values.push_back( std::to_integer< UInt8 >( byte ) );
    return values;
}

template < class Dto >
void ExpectRoundTrip( const Dto& source )
{
    std::vector< Byte > bytes;
    source.Write( bytes );
    ASSERT_FALSE( bytes.empty() );

    std::span< const Byte > cursor{ bytes };
    Dto restored;
    ASSERT_TRUE( restored.Read( cursor ) );
    EXPECT_TRUE( restored == source );
    EXPECT_TRUE( cursor.empty() ) << "Read left " << cursor.size() << " byte(s) unconsumed";
}

// [CS 4.4] 진부분 접두는 모두 거부. 감산 전 잔여 검사로 wrap 방지
template < class Dto >
void ExpectTruncationRejected( const Dto& source )
{
    std::vector< Byte > bytes;
    source.Write( bytes );

    for ( std::size_t length = 0; length < bytes.size(); ++length )
    {
        std::span< const Byte > cursor{ bytes.data(), length };
        const Dto untouched;
        Dto target;
        // [AD 11.2a] EXPECT_THROW 없음. 파싱 실패는 반환값이지 예외가 아님
        EXPECT_FALSE( target.Read( cursor ) )
            << "accepted a buffer truncated to " << length << " byte(s)";
        EXPECT_EQ( cursor.size(), length ) << "failed Read advanced the span at length " << length;
        EXPECT_TRUE( target == untouched )
            << "failed Read mutated the destination at length " << length;
    }
}

MoveInputRequest SampleMoveInput()
{
    MoveInputRequest dto;
    dto.client_tick_ = UInt32{ 0x01020304 };
    dto.direction_ = MoveDirection::East;
    dto.grid_x_ = Int16{ -2 };
    dto.grid_y_ = Int16{ 258 };
    dto.sprinting_ = true;
    return dto;
}

MoveStateBroadcast SampleMoveState()
{
    MoveStateBroadcast dto;
    dto.server_tick_ = UInt32{ 0xDEADBEEF };
    dto.actor_ids_ = { UInt64{ 1 }, UInt64{ 0xFFFFFFFFFFFFFFFFULL }, UInt64{ 42 } };
    dto.grid_xs_ = { Int16{ 0 }, Int16{ -32768 }, Int16{ 32767 } };
    dto.grid_ys_ = { Int16{ 7 }, Int16{ -1 }, Int16{ 0 } };
    return dto;
}

ChatBroadcast SampleChat()
{
    ChatBroadcast dto;
    dto.speaker_actor_id_ = UInt64{ 0x0102030405060708ULL };
    dto.channel_ = ChatChannel::World;
    dto.text_ = "hi";
    return dto;
}

// =============================================================================
// 1. 왕복
// =============================================================================

TEST( PktRoundTrip, MoveInputRequest ) { ExpectRoundTrip( SampleMoveInput() ); }

TEST( PktRoundTrip, MoveStateBroadcastWithRepeatedFields ) { ExpectRoundTrip( SampleMoveState() ); }

TEST( PktRoundTrip, ChatBroadcastWithText ) { ExpectRoundTrip( SampleChat() ); }

TEST( PktRoundTrip, DefaultConstructedIsStable )
{
    ExpectRoundTrip( MoveInputRequest{} );
    ExpectRoundTrip( MoveStateBroadcast{} );
    ExpectRoundTrip( ChatBroadcast{} );
}

TEST( PktRoundTrip, ReadStopsAtTheEndOfItsOwnPayload )
{
    // 첫 Read 가 자기 바이트만 소비해야 프레임 안에서 DTO 를 쓸 수 있음
    std::vector< Byte > bytes;
    const MoveInputRequest first = SampleMoveInput();
    const ChatBroadcast second = SampleChat();
    first.Write( bytes );
    second.Write( bytes );

    std::span< const Byte > cursor{ bytes };
    MoveInputRequest restored_first;
    ChatBroadcast restored_second;
    ASSERT_TRUE( restored_first.Read( cursor ) );
    ASSERT_TRUE( restored_second.Read( cursor ) );
    EXPECT_TRUE( restored_first == first );
    EXPECT_TRUE( restored_second == second );
    EXPECT_TRUE( cursor.empty() );
}

// =============================================================================
// 2. 절단 입력은 false
// =============================================================================

TEST( PktTruncation, MoveInputRequest ) { ExpectTruncationRejected( SampleMoveInput() ); }

TEST( PktTruncation, MoveStateBroadcastWithRepeatedFields )
{
    ExpectTruncationRejected( SampleMoveState() );
}

TEST( PktTruncation, ChatBroadcastWithText ) { ExpectTruncationRejected( SampleChat() ); }

TEST( PktTruncation, LyingLengthPrefixIsRejectedNotAllocated )
{
    // 뒤가 빈 65535 개 주장 접두는 할당이 아니라 버퍼 검사에서 실패해야 함
    // 여기서 멈추거나 죽으면 코덱이 예약을 시작한 것
    std::vector< Byte > bytes = ToBytes( { 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF } );
    std::span< const Byte > cursor{ bytes };
    MoveStateBroadcast target;
    EXPECT_FALSE( target.Read( cursor ) );

    std::vector< Byte > text_bytes =
        ToBytes( { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10 } );
    std::span< const Byte > text_cursor{ text_bytes };
    ChatBroadcast chat_target;
    EXPECT_FALSE( chat_target.Read( text_cursor ) );
}

// =============================================================================
// 3. 리틀 엔디안 바이트 순서
// =============================================================================

TEST( PktByteOrder, MoveInputRequestIsLittleEndian )
{
    std::vector< Byte > bytes;
    SampleMoveInput().Write( bytes );

    const std::vector< UInt8 > expected = {
        0x04, 0x03, 0x02, 0x01,  // UInt32 client_tick_, 최하위 바이트 먼저
        0x02,                    // MoveDirection::East, 기반 타입 UInt8 이라 1바이트
        0xFE, 0xFF,              // Int16 grid_x_ = -2 -> 0xFFFE
        0x02, 0x01,              // Int16 grid_y_ = 258 -> 0x0102
        0x01,                    // bool sprinting_ = true, 정확히 1바이트
    };
    EXPECT_EQ( FromBytes( bytes ), expected );
}

TEST( PktByteOrder, ChatBroadcastPrefixesTextWithUInt16ByteCount )
{
    std::vector< Byte > bytes;
    SampleChat().Write( bytes );

    const std::vector< UInt8 > expected = {
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,  // UInt64 speaker id, 리틀 엔디안
        0x01,                                            // ChatChannel::World
        0x02, 0x00,                                      // UInt16 길이 접두 = 2바이트
        0x68, 0x69,                                      // "hi" UTF-8
    };
    EXPECT_EQ( FromBytes( bytes ), expected );
}

TEST( PktByteOrder, TextPrefixCountsBytesNotCodePoints )
{
    // [AD 8.5] U+AC00 은 UTF-8 로 3바이트. 접두는 바이트를 세므로 1 이 아니라 3
    // 컴파일러 소스 문자셋에 결과가 좌우되지 않도록 이스케이프로 표기
    ChatBroadcast dto;
    dto.text_ = std::string( "\xEA\xB0\x80" );
    ASSERT_EQ( dto.text_.size(), std::size_t{ 3 } );

    std::vector< Byte > bytes;
    dto.Write( bytes );
    const std::vector< UInt8 > expected = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // speaker id 0
        0x00,                                            // ChatChannel::Say
        0x03, 0x00,                                      // UInt16 접두 = 3바이트, 1글자가 아님
        0xEA, 0xB0, 0x80,                                // U+AC00 UTF-8
    };
    EXPECT_EQ( FromBytes( bytes ), expected );

    ExpectRoundTrip( dto );
}

TEST( PktByteOrder, RepeatedFieldIsCountPrefixedThenElements )
{
    MoveStateBroadcast dto;
    dto.server_tick_ = UInt32{ 0 };
    dto.actor_ids_ = { UInt64{ 0x0807060504030201ULL } };

    std::vector< Byte > bytes;
    dto.Write( bytes );
    const std::vector< UInt8 > expected = {
        0x00, 0x00, 0x00, 0x00,                          // server_tick_
        0x01, 0x00,                                      // UInt16 개수 접두 = 1
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // UInt64, 리틀 엔디안
        0x00, 0x00,                                      // grid_xs_ 개수 = 0
        0x00, 0x00,                                      // grid_ys_ 개수 = 0
    };
    EXPECT_EQ( FromBytes( bytes ), expected );
}

TEST( PktByteOrder, EnumWidthMatchesTheContract )
{
    // MoveDirection 은 계약상 : byte. 기반 타입을 놓치면 이 크기가 움직임
    std::vector< Byte > narrow;
    std::vector< Byte > wide;
    MoveInputRequest dto;
    dto.direction_ = MoveDirection::None;
    dto.Write( narrow );
    dto.direction_ = MoveDirection::West;
    dto.Write( wide );
    ASSERT_EQ( narrow.size(), wide.size() );
    EXPECT_EQ( FromBytes( wide )[4], UInt8{ 0x04 } );
}

}  // namespace
