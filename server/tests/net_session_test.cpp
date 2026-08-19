// =============================================================================
// 실제 소켓 위의 세션 계층 검사. strand 직렬화, 예외 봉쇄, 수명, 유휴 타이머
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

#include "atlas/core/error.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/net/acceptor.h"
#include "atlas/net/io_runner.h"
#include "atlas/net/net_types.h"
#include "atlas/net/session.h"

namespace
{

using atlas::Byte;

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

// 이 파일의 모든 대기에 상한. 회귀가 CI 멈춤이 아니라 단언 실패로 드러남
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

    ~TestClient() { Disconnect(); }

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

    void Disconnect()
    {
        atlas::ErrorCode ignored;
        std::ignore = socket_.shutdown( atlas::Socket::shutdown_both, ignored );
        std::ignore = socket_.close( ignored );
    }

private:
    atlas::IoContext io_context_;
    atlas::Socket socket_;
};

// 실제 acceptor / session / runner 스택 위의 에코 서버
// 이 계층엔 프레이밍이 없어 왕복이 주장할 수 있는 것은 같은 바이트뿐
class NetSessionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        for ( std::size_t i = 0; i < kMaxTrackedSessions; ++i )
        {
            accepted_futures_[i] = accepted_[i].get_future();
        }

        // 워커 2개. 직렬화에 실패한 strand 가 실제로 관측되도록
        runner_ = std::make_unique< atlas::IoRunner >( 2 );
        // Tcp::v4() 가 아닌 loopback. any 주소는 local_endpoint() 가 0.0.0.0
        // Windows 는 그리로의 connect 를 WSAEADDRNOTAVAIL 로 거부한다
        // 포트 0 은 OS 가 빈 포트를 고르게 해 병렬 ctest 충돌을 막는다
        acceptor_ = std::make_unique< atlas::SessionAcceptor >(
            runner_->Context(), atlas::Endpoint( atlas::asio::ip::address_v4::loopback(), 0 ),
            [this]( const std::shared_ptr< atlas::Session >& session ) { OnAccept( session ); },
            idle_timeout_ );
        acceptor_->Start();
        runner_->Start();
    }

    void TearDown() override
    {
        // [AD 9] 기록된 종료 순서. 나머지는 역선언순 소멸이 알아서 처리
        acceptor_->Stop();
        runner_->Stop();
    }

    void OnAccept( const std::shared_ptr< atlas::Session >& session )
    {
        session->SetBytesHandler(
            []( const std::shared_ptr< atlas::Session >& peer, std::span< const Byte > bytes )
            {
                // [AD 11.2] 봉쇄 방아쇠. 핸들러가 I/O 스레드를 데려가면 안 됨
                if ( !bytes.empty() && bytes.front() == static_cast< Byte >( 'X' ) )
                {
                    ATLAS_THROW( atlas::Exception, "deliberate handler failure" );
                }
                peer->Send( bytes );
            } );
        session->SetCloseHandler( [this]( const std::shared_ptr< atlas::Session >& )
                                  { closed_count_.fetch_add( 1 ); } );

        // accept strand 위라 카운터에 별도 동기화 불필요
        const std::size_t index = accepted_count_++;
        if ( index < kMaxTrackedSessions )
        {
            accepted_[index].set_value( session );
        }
    }

    // 인덱스당 1회. 값을 옮겨 내야 수명 테스트에서 핸들러가 유일한 소유자가 됨
    std::shared_ptr< atlas::Session > TakeSession( std::size_t index )
    {
        if ( accepted_futures_[index].wait_for( atlas::Seconds{ 5 } ) != std::future_status::ready )
        {
            return nullptr;
        }
        return accepted_futures_[index].get();
    }

    static constexpr std::size_t kMaxTrackedSessions = 4;

    // 이 픽스처가 받는 세션의 유휴 타임아웃. 유휴 픽스처는 생성자에서 줄임
    atlas::Duration idle_timeout_{ atlas::Session::kDefaultIdleTimeout };

    // 선언 순서가 곧 소멸 계약. runner_ 가 마지막
    std::unique_ptr< atlas::IoRunner > runner_;
    std::unique_ptr< atlas::SessionAcceptor > acceptor_;
    std::array< std::promise< std::shared_ptr< atlas::Session > >, kMaxTrackedSessions > accepted_;
    std::array< std::future< std::shared_ptr< atlas::Session > >, kMaxTrackedSessions >
        accepted_futures_;
    std::size_t accepted_count_{ 0 };
    std::atomic< atlas::UInt32 > closed_count_{ 0 };
};

TEST_F( NetSessionTest, RoundTripsRawBytes )
{
    TestClient client( acceptor_->LocalEndpoint() );

    const std::vector< Byte > payload = MakeBytes( "atlas raw byte stream" );
    client.Write( payload );

    EXPECT_EQ( client.Read( payload.size() ), payload );
}

// [AD 9.1] 세션 상태에 락이 없는 근거
// strand 가 직렬화를 멈추면 최대 동시 진입이 1 을 넘는다
// 그때 Session 의 무잠금 멤버 전부가 데이터 경합이 된다
TEST_F( NetSessionTest, StrandSerialisesConcurrentPosts )
{
    TestClient client( acceptor_->LocalEndpoint() );
    const std::shared_ptr< atlas::Session > session = TakeSession( 0 );
    ASSERT_NE( session, nullptr );

    std::atomic< atlas::UInt32 > in_flight{ 0 };
    std::atomic< atlas::UInt32 > max_in_flight{ 0 };
    std::atomic< atlas::UInt32 > completed{ 0 };

    constexpr atlas::UInt32 kPosterThreads = 4;
    constexpr atlas::UInt32 kPostsPerThread = 250;

    std::vector< std::thread > posters;
    posters.reserve( kPosterThreads );
    for ( atlas::UInt32 thread_index = 0; thread_index < kPosterThreads; ++thread_index )
    {
        posters.emplace_back(
            [&]
            {
                for ( atlas::UInt32 post_index = 0; post_index < kPostsPerThread; ++post_index )
                {
                    atlas::asio::post(
                        session->GetStrand(),
                        [&]
                        {
                            const atlas::UInt32 now = in_flight.fetch_add( 1 ) + 1;
                            atlas::UInt32 observed = max_in_flight.load();
                            while ( now > observed &&
                                    !max_in_flight.compare_exchange_weak( observed, now ) )
                            {
                                // 실패 시 compare_exchange_weak 가 observed 를 갱신
                            }
                            // 깨진 strand 라면 두 번째 핸들러가 들어올 창을 넓힘
                            std::this_thread::yield();
                            in_flight.fetch_sub( 1 );
                            completed.fetch_add( 1 );
                        } );
                }
            } );
    }
    for ( std::thread& poster : posters )
    {
        poster.join();
    }

    ASSERT_TRUE(
        WaitUntil( [&] { return completed.load() == kPosterThreads * kPostsPerThread; } ) );
    EXPECT_EQ( max_in_flight.load(), atlas::UInt32{ 1 } );
}

// [AD 11.2] 회귀 방지선. 핸들러를 빠져나온 예외는 io_context::run() 에 닿는다
// 그러면 그 I/O 스레드가 죽고 그 스레드가 맡던 세션 전부가 침묵한다
// Guarded 가 그것을 막는다. IoRunner 에 두 번째 그물은 일부러 없다
TEST_F( NetSessionTest, HandlerExceptionDoesNotKillTheIoThread )
{
    const atlas::UInt64 errors_before = atlas::LogCount( atlas::LogLevel::Error );

    {
        TestClient thrower( acceptor_->LocalEndpoint() );
        const std::shared_ptr< atlas::Session > session = TakeSession( 0 );
        ASSERT_NE( session, nullptr );

        thrower.Write( MakeBytes( "X" ) );

        ASSERT_TRUE( WaitUntil( [&] { return closed_count_.load() >= 1; } ) );
    }

    EXPECT_GE( atlas::LogCount( atlas::LogLevel::Error ), errors_before + 1 );

    // 풀이 살아남았다는 증거. 새 연결이 여전히 서비스됨
    TestClient healthy( acceptor_->LocalEndpoint() );
    const std::vector< Byte > payload = MakeBytes( "still serving" );
    healthy.Write( payload );
    EXPECT_EQ( healthy.Read( payload.size() ), payload );
}

// [CS 4.4] enable_shared_from_this 필수. 소유자가 놓아도 진행 중 작업이 살림
TEST_F( NetSessionTest, InFlightHandlerKeepsTheSessionAlive )
{
    TestClient client( acceptor_->LocalEndpoint() );

    std::weak_ptr< atlas::Session > weak;
    {
        const std::shared_ptr< atlas::Session > session = TakeSession( 0 );
        ASSERT_NE( session, nullptr );
        weak = session;
    }

    // 테스트는 이제 아무것도 안 쥠. 대기 중인 async_read_some 이 쥠
    EXPECT_FALSE( weak.expired() );

    const std::vector< Byte > payload = MakeBytes( "ping" );
    client.Write( payload );
    EXPECT_EQ( client.Read( payload.size() ), payload );
    EXPECT_FALSE( weak.expired() );

    client.Disconnect();
    EXPECT_TRUE( WaitUntil( [&] { return closed_count_.load() >= 1; } ) );
}

// [AD 9.3] 유휴 타임아웃. Session 이 이를 파라미터로 받는 이유
class NetSessionIdleTest : public NetSessionTest
{
protected:
    // 일부러 더 죄지 않는다. 재무장 테스트가 한 창 안에서 반복 기록해야 한다
    // 스케줄러 잡음 수준 값은 바쁜 머신에서 그냥 실패한다
    static constexpr atlas::Duration kIdleTimeout = atlas::Millis{ 500 };

    NetSessionIdleTest() { idle_timeout_ = kIdleTimeout; }
};

// 이것이 막는 실패. 전원이 끊긴 피어는 FIN 을 보내지 않는다
// 그러면 이 계층의 무엇도 그 세션을 닫지 못한다
// 에러 없이 슬롯만 조용히 고갈된다
TEST_F( NetSessionIdleTest, SilentSessionIsClosedByTheIdleTimer )
{
    TestClient client( acceptor_->LocalEndpoint() );
    ASSERT_NE( TakeSession( 0 ), nullptr );

    // 한 바이트도 안 보내고 끊지도 않음. 닫을 수 있는 것은 타이머뿐
    EXPECT_TRUE( WaitUntil( [&] { return closed_count_.load() >= 1; } ) );
}

// 계속 말하는 세션이 닫히면 유휴 타임아웃이 연결 수명 상한으로 변질됨
TEST_F( NetSessionIdleTest, TrafficReArmsTheIdleTimer )
{
    TestClient client( acceptor_->LocalEndpoint() );
    ASSERT_NE( TakeSession( 0 ), nullptr );

    const std::vector< Byte > payload = MakeBytes( "tick" );
    // 100ms 간격 6회는 500ms 타임아웃보다 김. 재무장이 없었다면 이미 발화
    constexpr atlas::UInt32 kRounds = 6;
    for ( atlas::UInt32 round = 0; round < kRounds; ++round )
    {
        std::this_thread::sleep_for( atlas::Millis{ 100 } );
        client.Write( payload );
        ASSERT_EQ( client.Read( payload.size() ), payload );
    }

    EXPECT_EQ( closed_count_.load(), atlas::UInt32{ 0 } );
}

// 타이머가 강참조를 쥐어 닫힌 뒤에도 핸들러가 돎. 거기서 무동작이어야 함
// 두 번 닫으면 소유자에 두 번 통지되고, throw 하면 정상 종료에 ERROR 가 남음
TEST_F( NetSessionIdleTest, ClosedSessionIsNotDisturbedByTheTimer )
{
    TestClient client( acceptor_->LocalEndpoint() );
    const std::shared_ptr< atlas::Session > session = TakeSession( 0 );
    ASSERT_NE( session, nullptr );

    const atlas::UInt64 errors_before = atlas::LogCount( atlas::LogLevel::Error );

    session->Close();
    ASSERT_TRUE( WaitUntil( [&] { return closed_count_.load() >= 1; } ) );

    // Close() 가 취소하지 않았다면 타이머가 만료됐을 시점을 지남
    std::this_thread::sleep_for( kIdleTimeout + atlas::Millis{ 300 } );

    EXPECT_EQ( closed_count_.load(), atlas::UInt32{ 1 } );
    EXPECT_EQ( atlas::LogCount( atlas::LogLevel::Error ), errors_before );
}

TEST( NetIoRunner, GracefulStopDrainsTheQueueAndIsIdempotent )
{
    atlas::IoRunner runner;
    EXPECT_GT( runner.WorkerCount(), std::size_t{ 0 } );

    std::atomic< bool > ran{ false };
    runner.Start();
    atlas::asio::post( runner.Context(), [&] { ran.store( true ); } );

    // graceful = work guard 해제 후 큐 소진. Stop() 반환 시 핸들러는 실행 완료
    runner.Stop();
    EXPECT_TRUE( ran.load() );

    runner.Stop();
}

TEST( NetIoRunner, ExplicitWorkerCountIsHonoured )
{
    const atlas::IoRunner runner( 3 );
    EXPECT_EQ( runner.WorkerCount(), std::size_t{ 3 } );
}

}  // namespace
