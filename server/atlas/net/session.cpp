// =============================================================================
// Session 구현. strand 위의 읽기 루프, 유휴 타이머, FIFO 쓰기 체인, 단일 종료 경로
// =============================================================================

#include "atlas/net/session.h"

#include <tuple>
#include <utility>

#include "atlas/core/log.h"

namespace atlas
{

std::shared_ptr< Session > Session::Create( IoContext& io_context, Socket socket, SessionId id,
                                            Duration idle_timeout )
{
    // make_shared 는 private 생성자에 못 닿는다
    // passkey 타입은 여기서 아끼는 할당 1회보다 비싸다
    return std::shared_ptr< Session >(
        new Session( io_context, std::move( socket ), id, idle_timeout ) );
}

Session::Session( IoContext& io_context, Socket socket, SessionId id, Duration idle_timeout )
    : socket_( std::move( socket ) ),
      strand_( asio::make_strand( io_context ) ),
      id_( id ),
      idle_timer_( io_context ),
      idle_timeout_( idle_timeout )
{
    // [AD 9.2] 원장이 세션을 따라다닌다
    // 모든 로그와 guarded 핸들러 안의 예외가 어느 연결인지 이미 달고 있다
    ctx_.session_id = id;
}

Session::~Session() = default;

void Session::SetBytesHandler( BytesHandler handler ) { bytes_handler_ = std::move( handler ); }

void Session::SetCloseHandler( CloseHandler handler ) { close_handler_ = std::move( handler ); }

void Session::Start()
{
    auto self = shared_from_this();
    asio::post( strand_, Guarded(
                             ctx_,
                             [self]
                             {
                                 self->ArmIdleTimerOnStrand();
                                 self->StartReadOnStrand();
                             },
                             MakeFailureHandler() ) );
}

void Session::Send( std::span< const Byte > bytes )
{
    // 쓰기 시점이 아니라 여기서 복사한다
    // bytes 는 strand 가 도는 시점엔 사라진 임시일 수 있다
    std::vector< Byte > payload( bytes.begin(), bytes.end() );
    auto self = shared_from_this();
    asio::post( strand_,
                Guarded(
                    ctx_, [self, payload = std::move( payload )]() mutable
                    { self->EnqueueOnStrand( std::move( payload ) ); }, MakeFailureHandler() ) );
}

void Session::Close()
{
    auto self = shared_from_this();
    asio::post( strand_, Guarded(
                             ctx_, [self] { self->CloseOnStrand( "closed by owner" ); },
                             MakeFailureHandler() ) );
}

FailureHandler Session::MakeFailureHandler()
{
    // 강한 참조 캡처
    // 가드는 핸들러 본문이 이미 실패한 뒤에 돈다
    // 그때도 연결은 내려야 한다
    return { [self = shared_from_this()]( std::string_view reason )
             {
                 // 이미 strand 위 - Guarded::Fail 은 감싼 핸들러 안에서 실행됨
                 self->CloseOnStrand( reason );
             } };
}

void Session::StartReadOnStrand()
{
    if ( closed_ )
    {
        return;
    }

    auto self = shared_from_this();
    socket_.async_read_some(
        asio::buffer( read_buffer_ ),
        asio::bind_executor(
            strand_, Guarded(
                         ctx_, [self]( const ErrorCode& ec, std::size_t bytes_read )
                         { self->OnReadOnStrand( ec, bytes_read ); }, MakeFailureHandler() ) ) );
}

void Session::OnReadOnStrand( const ErrorCode& ec, std::size_t bytes_read )
{
    if ( ec )
    {
        // [AD 11.2a] 피어가 사라지는 것은 예상된 결과라 예외가 아니라 error_code
        CloseOnStrand( ec.message() );
        return;
    }

    // 바이트 핸들러 전에 재무장한다
    // 핸들러는 소유자 코드라 오래 걸릴 수 있다
    // 그 시간은 침묵이 아니다
    ArmIdleTimerOnStrand();

    if ( bytes_handler_ )
    {
        // [AD 8] 소켓이 뱉은 그대로의 바이트
        // 내용 해석은 프레임 계층의 몫
        bytes_handler_( shared_from_this(),
                        std::span< const Byte >( read_buffer_.data(), bytes_read ) );
    }

    StartReadOnStrand();
}

void Session::ArmIdleTimerOnStrand()
{
    if ( closed_ )
    {
        return;
    }

    // expires_after 가 걸려 있던 대기를 취소하므로 대기가 둘 남는 일이 없음
    idle_timer_.expires_after( idle_timeout_ );

    auto self = shared_from_this();
    idle_timer_.async_wait( asio::bind_executor(
        strand_, Guarded(
                     ctx_, [self]( const ErrorCode& ec ) { self->OnIdleTimeoutOnStrand( ec ); },
                     MakeFailureHandler() ) ) );
}

void Session::OnIdleTimeoutOnStrand( const ErrorCode& ec )
{
    // 재무장과 Close() 둘 다 대기를 취소함. 정상 결과이지 실패가 아님
    if ( ec == asio::error::operation_aborted )
    {
        return;
    }

    // strand 가 이 검사를 불필요하게 만들지 않는다
    // 발화한 타이머는 성공 코드로 핸들러를 큐잉한다
    // 그 사이 읽기가 완료돼 재무장하면 방금 말을 건 연결을 닫는다
    // 되읽은 만료가 미래면 낡은 발화이므로 여기서 버린다
    if ( idle_timer_.expiry() > Clock::now() )
    {
        return;
    }

    if ( closed_ )
    {
        return;
    }

    // 단일 종료 경로 재사용. 타임아웃은 새로운 종류의 종료가 아님
    CloseOnStrand( "idle timeout" );
}

void Session::EnqueueOnStrand( std::vector< Byte >&& payload )
{
    if ( closed_ || payload.empty() )
    {
        return;
    }

    // [AD 9] 백프레셔. 합이 넘칠 수 없도록 더하기 대신 상한에서 빼는 형태로 씀
    // 불변식 write_queue_bytes_ <= kMaxWriteQueueBytes 가 좌변을 안전하게 만듦
    // 폐기가 아니라 연결 종료 - 조용한 폐기는 프로토콜을 소리 없이 깸
    if ( payload.size() > kMaxWriteQueueBytes - write_queue_bytes_ )
    {
        CloseOnStrand( "write queue above kMaxWriteQueueBytes" );
        return;
    }

    // 빈 큐 = 진행 중 쓰기 없음이라 이번 것이 체인을 시작해야 함
    const bool idle = write_queue_.empty();
    write_queue_bytes_ += payload.size();
    write_queue_.push_back( std::move( payload ) );
    if ( idle )
    {
        StartWriteOnStrand();
    }
}

void Session::StartWriteOnStrand()
{
    auto self = shared_from_this();
    asio::async_write(
        socket_, asio::buffer( write_queue_.front() ),
        asio::bind_executor( strand_,
                             Guarded(
                                 ctx_, [self]( const ErrorCode& ec, std::size_t )
                                 { self->OnWriteOnStrand( ec ); }, MakeFailureHandler() ) ) );
}

void Session::OnWriteOnStrand( const ErrorCode& ec )
{
    if ( ec )
    {
        CloseOnStrand( ec.message() );
        return;
    }

    // 성공한 쓰기와 실패한 읽기가 같은 strand 배치에서 완료될 수 있다
    // 그때 읽기가 먼저 돈다
    // 이 가드가 없으면 보낼 것이 없는 세션에서 아래 큐를 훑는다
    if ( closed_ )
    {
        return;
    }

    write_queue_bytes_ -= write_queue_.front().size();
    write_queue_.pop_front();
    if ( !write_queue_.empty() )
    {
        StartWriteOnStrand();
    }
}

// reason 은 DEBUG 줄에만 쓰이고 DEBUG 는 릴리스에서 컴파일 아웃된다
// 이 속성이 사용처의 캐스트 없이 릴리스 경고를 막아 준다
void Session::CloseOnStrand( [[maybe_unused]] std::string_view reason )
{
    if ( closed_ )
    {
        return;
    }
    closed_ = true;

    // 대기 취소. 핸들러는 operation_aborted 로 여전히 돌고 강한 참조를 쥐고 있음
    idle_timer_.cancel();

    // 피어가 이미 끊은 소켓의 shutdown 은 실패하고, 종료 경로가 반응할 일이 아님
    ErrorCode ignored;
    std::ignore = socket_.shutdown( Socket::shutdown_both, ignored );
    std::ignore = socket_.close( ignored );

    // 큐를 여기서 비우지 않는 것은 의도
    // close() 는 진행 중 async_write 를 취소하지만 취소는 비동기다
    // 완료 핸들러가 돌 때까지 앞 버퍼는 OS 에 등록된 채로 남는다
    // 핸들러가 shared_from_this() 를 쥐어 세션과 큐가 미완 연산보다 오래 산다

    ATLAS_LOG_DEBUG( "session {} closed: {}", IdValue( id_ ), reason );

    if ( close_handler_ )
    {
        // 먼저 옮겨 두어 콜백이 재진입해도 통지가 두 번 나가지 않음
        const CloseHandler handler = std::move( close_handler_ );
        close_handler_ = nullptr;
        handler( shared_from_this() );
    }
}

}  // namespace atlas
