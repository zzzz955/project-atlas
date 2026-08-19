// =============================================================================
// SessionAcceptor 구현. accept strand 위의 수락 루프와 지연 재시도
// =============================================================================

#include "atlas/net/acceptor.h"

#include <string_view>
#include <tuple>
#include <utility>

#include "atlas/core/error.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"

namespace atlas
{

SessionAcceptor::SessionAcceptor( IoContext& io_context, const Endpoint& endpoint,
                                  AcceptHandler on_accept, Duration session_idle_timeout )
    : io_context_( io_context ),
      acceptor_( io_context ),
      strand_( asio::make_strand( io_context ) ),
      retry_timer_( io_context ),
      accept_handler_( std::move( on_accept ) ),
      session_idle_timeout_( session_idle_timeout )
{
    acceptor_.open( endpoint.protocol() );
    // 없으면 재시작한 서버가 TIME_WAIT 인 포트를 다시 잡지 못함
    acceptor_.set_option( Acceptor::reuse_address( true ) );
    acceptor_.bind( endpoint );
    acceptor_.listen();
    local_endpoint_ = acceptor_.local_endpoint();
}

SessionAcceptor::~SessionAcceptor() = default;

void SessionAcceptor::Start()
{
    asio::post( strand_, Guarded( ctx_, [this] { StartAcceptOnStrand(); } ) );
    ATLAS_LOG_INFO( "acceptor listening on {}:{}", local_endpoint_.address().to_string(),
                    local_endpoint_.port() );
}

void SessionAcceptor::Stop()
{
    asio::post( strand_, Guarded( ctx_, [this] { CloseOnStrand(); } ) );
}

void SessionAcceptor::StartAcceptOnStrand()
{
    if ( closed_ )
    {
        return;
    }

    // [AD 11.2b] 실패 훅이 루프를 재무장한다
    // 없으면 소유자 콜백의 throw 한 번에 리스너가 조용히 죽는다
    // 프로세스는 멀쩡해 보이는데 전면 장애가 된다
    acceptor_.async_accept( asio::bind_executor(
        strand_, Guarded(
                     ctx_, [this]( const ErrorCode& ec, Socket socket )
                     { OnAcceptOnStrand( ec, std::move( socket ) ); },
                     FailureHandler( [this]( std::string_view ) { StartAcceptOnStrand(); } ) ) ) );
}

void SessionAcceptor::OnAcceptOnStrand( const ErrorCode& ec, Socket socket )
{
    if ( ec )
    {
        // Stop() 이 취소한 accept. 정상 종료 경로이고 재무장하면 리스너가 되살아남
        if ( ec == asio::error::operation_aborted )
        {
            return;
        }
        // 나머지는 연결 단위 실패. 지연 후 재무장하는 이유는 kAcceptRetryDelay 참고
        ATLAS_LOG_WARN( "accept failed: {}", ec.message() );
        RetryAcceptOnStrand();
        return;
    }

    // Nagle 은 작은 쓰기를 뭉친다
    // 지연에 민감한 게임 연결이라 첫 바이트부터 끈다
    // 실패해도 느릴 뿐 동작하므로 WARN 만 남기고 진행
    ErrorCode nodelay_ec;
    std::ignore = socket.set_option( Tcp::no_delay( true ), nodelay_ec );
    if ( nodelay_ec )
    {
        ATLAS_LOG_WARN( "TCP_NODELAY not set: {}", nodelay_ec.message() );
    }

    const SessionId id{ next_session_id_++ };
    auto session = Session::Create( io_context_, std::move( socket ), id, session_idle_timeout_ );
    if ( accept_handler_ )
    {
        accept_handler_( session );
    }
    // 콜백 뒤에 시작. 첫 바이트가 도착하기 전에 소유자 핸들러가 설치되어 있어야 함
    session->Start();

    ATLAS_LOG_DEBUG( "session {} accepted", IdValue( id ) );
    StartAcceptOnStrand();
}

void SessionAcceptor::RetryAcceptOnStrand()
{
    if ( closed_ )
    {
        return;
    }

    retry_timer_.expires_after( kAcceptRetryDelay );
    retry_timer_.async_wait( asio::bind_executor(
        strand_,
        Guarded(
            ctx_,
            [this]( const ErrorCode& timer_ec )
            {
                // Stop() 이 이 대기를 취소한다
                // 여기서 재무장하면 리스너가 되살아난다
                // OnAcceptOnStrand 가 반환하는 것과 같은 이유
                if ( timer_ec == asio::error::operation_aborted )
                {
                    return;
                }
                StartAcceptOnStrand();
            },
            // 여기서 throw 가 나도 리스너는 계속 살아야 함 - 이유는 StartAcceptOnStrand 주석 참고
            FailureHandler( [this]( std::string_view ) { StartAcceptOnStrand(); } ) ) ) );
}

void SessionAcceptor::CloseOnStrand()
{
    if ( closed_ )
    {
        return;
    }
    closed_ = true;

    // 지연 재시도가 떠 있으면 종료 뒤에 accept 루프를 되살림
    retry_timer_.cancel();

    // boost 동기 연산은 실패를 out 파라미터와 반환값 양쪽으로 보고한다
    // 반환값 discard 를 명시하지 않으면 의도적 폐기와 누락을 구분할 수 없다
    ErrorCode ignored;
    std::ignore = acceptor_.cancel( ignored );
    std::ignore = acceptor_.close( ignored );
    ATLAS_LOG_INFO( "acceptor stopped on port {}", local_endpoint_.port() );
}

}  // namespace atlas
