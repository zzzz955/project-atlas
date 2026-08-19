// =============================================================================
// crc32, 프레임 인코딩, 재조립 상태 기계, 실제 소켓 위의 봉합면 검사
// =============================================================================

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <future>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/net/acceptor.h"
#include "atlas/net/io_runner.h"
#include "atlas/net/net_types.h"
#include "atlas/net/session.h"
#include "atlas/proto/crc32.h"
#include "atlas/proto/frame.h"
#include "atlas/proto/frame_reader.h"
#include "atlas/proto/session_framing.h"

namespace
{

using atlas::Byte;
using atlas::Frame;
using atlas::FrameError;
using atlas::FrameReader;
using atlas::UInt16;
using atlas::UInt32;

std::vector< Byte > MakeBytes( std::string_view text )
{
    std::vector< Byte > bytes;
    bytes.reserve( text.size() );
    for ( const char letter : text )
    {
        bytes.push_back( static_cast< Byte >( letter ) );
    }
    return bytes;
}

std::vector< Byte > MakeFrame( UInt16 opcode, UInt32 seq, std::span< const Byte > payload )
{
    std::vector< Byte > out;
    EXPECT_TRUE( atlas::EncodeFrame( out, opcode, seq, payload ) );
    return out;
}

// 인코더가 거부할 length 를 직접 써넣기 위한 손수 헤더. 적대적 피어 역할
std::vector< Byte > MakeHeader( UInt16 length, UInt16 opcode, UInt32 seq, UInt32 crc32 )
{
    std::vector< Byte > out;
    const auto push16 = [&out]( UInt16 value )
    {
        out.push_back( static_cast< Byte >( value & 0xFFU ) );
        out.push_back( static_cast< Byte >( ( value >> 8U ) & 0xFFU ) );
    };
    const auto push32 = [&out]( UInt32 value )
    {
        out.push_back( static_cast< Byte >( value & 0xFFU ) );
        out.push_back( static_cast< Byte >( ( value >> 8U ) & 0xFFU ) );
        out.push_back( static_cast< Byte >( ( value >> 16U ) & 0xFFU ) );
        out.push_back( static_cast< Byte >( ( value >> 24U ) & 0xFFU ) );
    };
    push16( length );
    push16( opcode );
    push32( seq );
    push32( crc32 );
    return out;
}

std::span< const Byte > Slice( const std::vector< Byte >& bytes, std::size_t offset,
                               std::size_t count )
{
    return std::span< const Byte >( bytes ).subspan( offset, count );
}

// =============================================================================
// crc32 와 프레임 인코딩
// =============================================================================

// [AD 8.1] CRC-32/ISO-HDLC 표준 검사값
// 비트 순서가 틀린 테이블도 안정적이고 자기 일관된 체크섬을 낸다
// 공개 벡터만이 그 실수를 잡는다
TEST( ProtoCrc32, MatchesTheCanonicalCheckValue )
{
    const std::vector< Byte > data = MakeBytes( "123456789" );
    const UInt32 state = atlas::Crc32Update( atlas::kCrc32Init, data );
    EXPECT_EQ( atlas::Crc32Finish( state ), UInt32{ 0xCBF43926U } );
}

// 체크섬이 opcode + seq 까지 덮지 않으면 seq 가 손상된 리플레이가 살아남음
TEST( ProtoCrc32, ChecksumCoversTheHeaderFieldsNotJustThePayload )
{
    const std::vector< Byte > payload = MakeBytes( "same payload" );
    EXPECT_NE( atlas::FrameChecksum( 7, 1, payload ), atlas::FrameChecksum( 7, 2, payload ) );
    EXPECT_NE( atlas::FrameChecksum( 7, 1, payload ), atlas::FrameChecksum( 8, 1, payload ) );
}

TEST( ProtoFrame, EncodesTheFixedTwelveByteHeader )
{
    const std::vector< Byte > payload = MakeBytes( "hello" );
    const std::vector< Byte > wire = MakeFrame( 0x1234, 0xDEADBEEF, payload );

    ASSERT_EQ( wire.size(), atlas::kFrameHeaderSize + payload.size() );

    atlas::FrameHeader header{};
    ASSERT_TRUE( atlas::DecodeFrameHeader( wire, header ) );
    EXPECT_EQ( header.length, UInt16{ 5 } );  // 페이로드만 셈. 헤더 불포함
    EXPECT_EQ( header.opcode, UInt16{ 0x1234 } );
    EXPECT_EQ( header.seq, UInt32{ 0xDEADBEEF } );
    EXPECT_EQ( header.crc32, atlas::FrameChecksum( 0x1234, 0xDEADBEEF, payload ) );
}

TEST( ProtoFrame, DecodeHeaderRefusesATruncatedBuffer )
{
    const std::vector< Byte > wire = MakeFrame( 1, 1, MakeBytes( "x" ) );
    atlas::FrameHeader header{};
    EXPECT_FALSE(
        atlas::DecodeFrameHeader( Slice( wire, 0, atlas::kFrameHeaderSize - 1 ), header ) );
}

TEST( ProtoFrame, EncodeRefusesAPayloadAboveTheCap )
{
    const std::vector< Byte > payload( atlas::kMaxPayload + 1, Byte{ 0 } );
    std::vector< Byte > out;
    EXPECT_FALSE( atlas::EncodeFrame( out, 1, 1, payload ) );
    // 반쯤 쓰다 만 상태가 없을 것. 다음 프레임이 찌꺼기 접두부를 물려받지 않도록
    EXPECT_TRUE( out.empty() );
}

// =============================================================================
// FrameReader - 재조립 상태 기계
// =============================================================================
TEST( ProtoFrameReader, ParsesOneCompleteFrame )
{
    const std::vector< Byte > payload = MakeBytes( "atlas frame" );
    const std::vector< Byte > wire = MakeFrame( 42, 1, payload );

    FrameReader reader;
    std::vector< Frame > frames;
    ASSERT_EQ( reader.Feed( wire, frames ), FrameError::None );

    ASSERT_EQ( frames.size(), std::size_t{ 1 } );
    EXPECT_EQ( frames[0].opcode, UInt16{ 42 } );
    EXPECT_EQ( frames[0].seq, UInt32{ 1 } );
    EXPECT_EQ( frames[0].payload, payload );
    EXPECT_EQ( reader.Pending(), std::size_t{ 0 } );  // 붙들고 있는 바이트 없음
}

TEST( ProtoFrameReader, KeepsStateWhenTheHeaderIsSplit )
{
    const std::vector< Byte > payload = MakeBytes( "split header" );
    const std::vector< Byte > wire = MakeFrame( 9, 1, payload );

    FrameReader reader;
    std::vector< Frame > frames;

    // 5바이트. 헤더 중간이고 seq 는 시작도 하기 전
    ASSERT_EQ( reader.Feed( Slice( wire, 0, 5 ), frames ), FrameError::None );
    EXPECT_TRUE( frames.empty() );
    EXPECT_EQ( reader.Pending(), std::size_t{ 5 } );

    ASSERT_EQ( reader.Feed( Slice( wire, 5, wire.size() - 5 ), frames ), FrameError::None );
    ASSERT_EQ( frames.size(), std::size_t{ 1 } );
    EXPECT_EQ( frames[0].payload, payload );
    EXPECT_EQ( reader.Pending(), std::size_t{ 0 } );
}

TEST( ProtoFrameReader, KeepsStateWhenThePayloadIsSplit )
{
    const std::vector< Byte > payload = MakeBytes( "split payload across two reads" );
    const std::vector< Byte > wire = MakeFrame( 9, 1, payload );
    const std::size_t cut = atlas::kFrameHeaderSize + 4;

    FrameReader reader;
    std::vector< Frame > frames;

    ASSERT_EQ( reader.Feed( Slice( wire, 0, cut ), frames ), FrameError::None );
    EXPECT_TRUE( frames.empty() );
    EXPECT_EQ( reader.Pending(), cut );

    ASSERT_EQ( reader.Feed( Slice( wire, cut, wire.size() - cut ), frames ), FrameError::None );
    ASSERT_EQ( frames.size(), std::size_t{ 1 } );
    EXPECT_EQ( frames[0].payload, payload );
}

// 1바이트씩은 최악의 분할. 헤더의 모든 경계를 지나감
TEST( ProtoFrameReader, ReassemblesAByteAtATime )
{
    const std::vector< Byte > payload = MakeBytes( "drip" );
    const std::vector< Byte > wire = MakeFrame( 3, 1, payload );

    FrameReader reader;
    std::vector< Frame > frames;
    for ( std::size_t i = 0; i < wire.size(); ++i )
    {
        ASSERT_EQ( reader.Feed( Slice( wire, i, 1 ), frames ), FrameError::None );
        EXPECT_EQ( frames.size(), i + 1 == wire.size() ? std::size_t{ 1 } : std::size_t{ 0 } );
    }
    ASSERT_EQ( frames.size(), std::size_t{ 1 } );
    EXPECT_EQ( frames[0].payload, payload );
}

TEST( ProtoFrameReader, ParsesThreeFramesFromOneBufferInOrder )
{
    const std::array< std::string_view, 3 > texts{ "one", "two", "three" };
    std::vector< Byte > wire;
    for ( UInt32 index = 1; index <= 3; ++index )
    {
        const std::vector< Byte > payload = MakeBytes( texts[index - 1] );
        const std::vector< Byte > one =
            MakeFrame( static_cast< UInt16 >( 100 + index ), index, payload );
        wire.insert( wire.end(), one.begin(), one.end() );
    }

    FrameReader reader;
    std::vector< Frame > frames;
    ASSERT_EQ( reader.Feed( wire, frames ), FrameError::None );

    ASSERT_EQ( frames.size(), std::size_t{ 3 } );
    EXPECT_EQ( frames[0].opcode, UInt16{ 101 } );
    EXPECT_EQ( frames[1].opcode, UInt16{ 102 } );
    EXPECT_EQ( frames[2].opcode, UInt16{ 103 } );
    EXPECT_EQ( frames[2].payload, MakeBytes( "three" ) );
    EXPECT_EQ( reader.Pending(), std::size_t{ 0 } );
}

// 상한 검사가 크기 비교와 subspan 보다 앞. 안 온 length 만큼 자리를 안 잡음
TEST( ProtoFrameReader, RejectsALengthAboveTheCap )
{
    const std::vector< Byte > wire =
        MakeHeader( static_cast< UInt16 >( atlas::kMaxPayload + 1 ), 1, 1, 0 );

    FrameReader reader;
    std::vector< Frame > frames;
    EXPECT_EQ( reader.Feed( wire, frames ), FrameError::PayloadTooLarge );
    EXPECT_TRUE( frames.empty() );
}

// 상한 안의 과장된 접두부는 에러가 아니라 아직 오는 중. 닫으면 멀쩡한 세션이 죽음
TEST( ProtoFrameReader, ALyingPrefixWithinTheCapOnlyWaits )
{
    std::vector< Byte > wire = MakeHeader( 1000, 1, 1, 0 );
    const std::vector< Byte > partial = MakeBytes( "only ten b" );
    wire.insert( wire.end(), partial.begin(), partial.end() );

    FrameReader reader;
    std::vector< Frame > frames;
    EXPECT_EQ( reader.Feed( wire, frames ), FrameError::None );
    EXPECT_TRUE( frames.empty() );
    EXPECT_EQ( reader.Pending(), wire.size() );
}

TEST( ProtoFrameReader, RejectsASingleFlippedBit )
{
    const std::vector< Byte > payload = MakeBytes( "integrity" );
    std::vector< Byte > wire = MakeFrame( 5, 1, payload );

    // 페이로드의 1비트만. 헤더 필드는 전부 온전
    wire[atlas::kFrameHeaderSize] = wire[atlas::kFrameHeaderSize] ^ Byte { 0x01 };

    FrameReader reader;
    std::vector< Frame > frames;
    EXPECT_EQ( reader.Feed( wire, frames ), FrameError::ChecksumMismatch );
    EXPECT_TRUE( frames.empty() );
}

TEST( ProtoFrameReader, RejectsARepeatedSequenceNumber )
{
    const std::vector< Byte > payload = MakeBytes( "replay" );
    const std::vector< Byte > first = MakeFrame( 1, 7, payload );

    FrameReader reader;
    std::vector< Frame > frames;
    ASSERT_EQ( reader.Feed( first, frames ), FrameError::None );
    ASSERT_EQ( frames.size(), std::size_t{ 1 } );

    EXPECT_EQ( reader.Feed( first, frames ), FrameError::SequenceRegression );
    EXPECT_EQ( frames.size(), std::size_t{ 1 } );  // 중복분은 전달되지 않음
}

TEST( ProtoFrameReader, RejectsASequenceNumberThatGoesBackwards )
{
    const std::vector< Byte > payload = MakeBytes( "out of order" );

    FrameReader reader;
    std::vector< Frame > frames;
    ASSERT_EQ( reader.Feed( MakeFrame( 1, 10, payload ), frames ), FrameError::None );
    EXPECT_EQ( reader.Feed( MakeFrame( 1, 9, payload ), frames ), FrameError::SequenceRegression );
    EXPECT_EQ( frames.size(), std::size_t{ 1 } );
}

// 나쁜 프레임 앞에서 해독된 것은 그대로 넘김. 버리면 피어가 이미 치른 일을 잃음
TEST( ProtoFrameReader, DeliversTheGoodFramesAheadOfAFailure )
{
    const std::vector< Byte > good = MakeFrame( 1, 1, MakeBytes( "good" ) );
    std::vector< Byte > bad = MakeFrame( 1, 2, MakeBytes( "bad" ) );
    bad[atlas::kFrameHeaderSize] = bad[atlas::kFrameHeaderSize] ^ Byte { 0x80 };

    std::vector< Byte > wire = good;
    wire.insert( wire.end(), bad.begin(), bad.end() );

    FrameReader reader;
    std::vector< Frame > frames;
    EXPECT_EQ( reader.Feed( wire, frames ), FrameError::ChecksumMismatch );
    ASSERT_EQ( frames.size(), std::size_t{ 1 } );
    EXPECT_EQ( frames[0].payload, MakeBytes( "good" ) );
}

TEST( ProtoFrameReader, AcceptsAMaximumSizedPayload )
{
    const std::vector< Byte > payload( atlas::kMaxPayload, Byte{ 0xAB } );
    const std::vector< Byte > wire = MakeFrame( 1, 1, payload );

    FrameReader reader;
    std::vector< Frame > frames;
    ASSERT_EQ( reader.Feed( wire, frames ), FrameError::None );
    ASSERT_EQ( frames.size(), std::size_t{ 1 } );
    EXPECT_EQ( frames[0].payload.size(), atlas::kMaxPayload );
}

// =============================================================================
// 봉합면 - 실제 소켓 스택 위에서 바이트 -> 프레임
// =============================================================================
template < class Predicate >
bool WaitUntil( Predicate predicate )
{
    const atlas::TimePoint deadline = atlas::Clock::now() + atlas::Seconds{ 5 };
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

    void Write( const std::vector< Byte >& bytes )
    {
        atlas::asio::write( socket_, atlas::asio::buffer( bytes ) );
    }

    std::vector< Byte > Read( std::size_t count )
    {
        std::vector< Byte > bytes( count );
        atlas::asio::read( socket_, atlas::asio::buffer( bytes ) );
        return bytes;
    }

private:
    atlas::IoContext io_context_;
    atlas::Socket socket_;
};

// 프레임을 말하는 에코 서버. 바이트 경계와 핸들러 사이엔 AttachFrameReader 뿐
class ProtoSessionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        accepted_future_ = accepted_.get_future();
        runner_ = std::make_unique< atlas::IoRunner >( 2 );
        // any 주소가 아닌 loopback, 포트 0. 병렬 ctest 끼리 충돌 방지
        acceptor_ = std::make_unique< atlas::SessionAcceptor >(
            runner_->Context(), atlas::Endpoint( atlas::asio::ip::address_v4::loopback(), 0 ),
            [this]( const std::shared_ptr< atlas::Session >& session ) { OnAccept( session ); } );
        acceptor_->Start();
        runner_->Start();
    }

    void TearDown() override
    {
        // [AD 9] 현관문 먼저, 그 다음 풀
        acceptor_->Stop();
        runner_->Stop();
    }

    void OnAccept( const std::shared_ptr< atlas::Session >& session )
    {
        // Session::Create 이후 Session::Start() 이전. 바이트 핸들러를 꽂을 구간
        atlas::AttachFrameReader(
            session,
            [this]( const std::shared_ptr< atlas::Session >& source, const Frame& frame )
            {
                received_.fetch_add( 1 );
                // [AD 8.3] 방향별 송신 카운터. 세션 strand 위라 동기화 불필요
                ++send_seq_;
                atlas::SendFrame( *source, frame.opcode, send_seq_, frame.payload );
            } );
        session->SetCloseHandler( [this]( const std::shared_ptr< atlas::Session >& )
                                  { closed_count_.fetch_add( 1 ); } );

        // accept strand 위. 테스트당 세션 1개라 평범한 플래그로 충분
        if ( !accepted_set_ )
        {
            accepted_set_ = true;
            accepted_.set_value( session );
        }
    }

    std::shared_ptr< atlas::Session > TakeSession()
    {
        if ( accepted_future_.wait_for( atlas::Seconds{ 5 } ) != std::future_status::ready )
        {
            return nullptr;
        }
        return accepted_future_.get();
    }

    // 선언 순서가 곧 소멸 계약. runner_ 가 마지막
    std::unique_ptr< atlas::IoRunner > runner_;
    std::unique_ptr< atlas::SessionAcceptor > acceptor_;
    std::promise< std::shared_ptr< atlas::Session > > accepted_;
    std::future< std::shared_ptr< atlas::Session > > accepted_future_;
    bool accepted_set_{ false };
    UInt32 send_seq_{ 0 };
    std::atomic< UInt32 > received_{ 0 };
    std::atomic< UInt32 > closed_count_{ 0 };
};

TEST_F( ProtoSessionTest, FramedRoundTripOverARealSocket )
{
    TestClient client( acceptor_->LocalEndpoint() );

    const std::vector< Byte > payload = MakeBytes( "bytes in, frames out" );
    client.Write( MakeFrame( 77, 1, payload ) );

    const std::vector< Byte > response = client.Read( atlas::kFrameHeaderSize + payload.size() );

    FrameReader reader;
    std::vector< Frame > frames;
    ASSERT_EQ( reader.Feed( response, frames ), FrameError::None );
    ASSERT_EQ( frames.size(), std::size_t{ 1 } );
    EXPECT_EQ( frames[0].opcode, UInt16{ 77 } );
    EXPECT_EQ( frames[0].payload, payload );
    EXPECT_EQ( received_.load(), UInt32{ 1 } );
}

// TCP 쓰기 1회로 보낸 프레임 2개. 소켓이 합친 것을 리더가 다시 나눔
TEST_F( ProtoSessionTest, MergedWritesAreSplitBackIntoFrames )
{
    TestClient client( acceptor_->LocalEndpoint() );

    std::vector< Byte > wire = MakeFrame( 1, 1, MakeBytes( "first" ) );
    const std::vector< Byte > second = MakeFrame( 2, 2, MakeBytes( "second" ) );
    wire.insert( wire.end(), second.begin(), second.end() );
    client.Write( wire );

    ASSERT_TRUE( WaitUntil( [this] { return received_.load() >= 2; } ) );
}

// [AD 8.1] 프레이밍 실패는 연결 종료. 로그만 찍고 넘어가지 않음을 소켓에서 증명
TEST_F( ProtoSessionTest, AChecksumFailureClosesTheConnection )
{
    TestClient client( acceptor_->LocalEndpoint() );

    std::vector< Byte > wire = MakeFrame( 1, 1, MakeBytes( "corrupt me" ) );
    wire[atlas::kFrameHeaderSize] = wire[atlas::kFrameHeaderSize] ^ Byte { 0x01 };
    client.Write( wire );

    EXPECT_TRUE( WaitUntil( [this] { return closed_count_.load() >= 1; } ) );
    EXPECT_EQ( received_.load(), UInt32{ 0 } );
}

// [AD 9] 역압 상한. 큐 천장을 넘는 페이로드 하나는 절대 못 들어간다
// 피어 수신 창의 협조 없이 판정을 관찰할 수 있다
// 판정은 드롭이 아니라 close
TEST_F( ProtoSessionTest, WriteQueueOverflowClosesTheConnection )
{
    TestClient client( acceptor_->LocalEndpoint() );
    const std::shared_ptr< atlas::Session > session = TakeSession();
    ASSERT_NE( session, nullptr );

    const std::vector< Byte > oversized( atlas::Session::kMaxWriteQueueBytes + 1, Byte{ 0 } );
    session->Send( oversized );

    EXPECT_TRUE( WaitUntil( [this] { return closed_count_.load() >= 1; } ) );
}

}  // namespace
