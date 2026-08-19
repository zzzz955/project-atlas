#pragma once

// =============================================================================
// [AD 5.1] 연결 종단. listen -> accept -> Session 생성 -> Start
// =============================================================================

#include <functional>
#include <memory>

#include "atlas/core/ctx.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/net/net_types.h"
#include "atlas/net/session.h"

namespace atlas
{

// 이름이 Acceptor 가 아닌 것은 net_types.h 의 별칭 때문이다
// 같은 네임스페이스에서 atlas::Acceptor 와 충돌한다
// [AD 9.1] accept 루프는 전용 strand
// 종료 순서 Stop() -> runner.Stop() -> 소멸
class SessionAcceptor
{
public:
    // Session 생성 후 Start 전에 accept strand 에서 호출
    // 소유자가 세션 콜백을 설치하고 소유권을 가져가는 창
    using AcceptHandler = std::function< void( const std::shared_ptr< Session >& ) >;

    // open/bind/listen 을 즉시 수행. 포트 확보 실패는 기동 실패라 error_code 가 아니라 throw
    // session_idle_timeout 이 인자인 이유는 테스트가 300s 를 기다릴 수 없기 때문
    SessionAcceptor( IoContext& io_context, const Endpoint& endpoint, AcceptHandler on_accept,
                     Duration session_idle_timeout = Session::kDefaultIdleTimeout );

    SessionAcceptor( const SessionAcceptor& ) = delete;
    SessionAcceptor& operator=( const SessionAcceptor& ) = delete;
    SessionAcceptor( SessionAcceptor&& ) = delete;
    SessionAcceptor& operator=( SessionAcceptor&& ) = delete;
    ~SessionAcceptor();

    void Start();

    void Stop();  // accept 중단. close 자체는 accept strand 위

    // 생성 시 1회 캡처라 소켓 조회가 없다
    // 다른 스레드에서 읽어도 accept 루프와 경쟁하지 않는다
    // port 0 을 요청했을 때 OS 가 고른 포트도 여기서 확인
    [[nodiscard]] const Endpoint& LocalEndpoint() const noexcept { return local_endpoint_; }

private:
    void StartAcceptOnStrand();
    void OnAcceptOnStrand( const ErrorCode& ec, Socket socket );
    void RetryAcceptOnStrand();
    void CloseOnStrand();

    // EMFILE/ENFILE 로 실패한 accept 는 즉시 또 실패한다
    // 지연 없이 재무장하면 WARN 폭주와 CPU 100% 가 된다
    // 리스너는 살아 있는데 프로세스는 못 쓰게 된다
    // 지수 백오프 아님 - 백오프 상태는 두 번째 상태 기계가 된다
    static constexpr Duration kAcceptRetryDelay = Millis{ 100 };

    IoContext& io_context_;
    Acceptor acceptor_;
    Strand strand_;
    SteadyTimer retry_timer_;  // accept strand 소유라 별도 락 불필요
    Endpoint local_endpoint_;
    AcceptHandler accept_handler_;
    Duration session_idle_timeout_;
    Ctx ctx_;
    // [AD 6] 3계층 식별자의 세션 계층. accept strand 전용이라 평범한 카운터로 충분
    UInt64 next_session_id_{ 1 };
    bool closed_{ false };
};

}  // namespace atlas
