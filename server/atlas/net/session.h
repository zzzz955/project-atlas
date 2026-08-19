#pragma once

// =============================================================================
// [AD 9] TCP 연결 1개 = Session 1개. strand 로 직렬화
// 경계는 바이트 스트림
// 프레임/프로토콜은 atlas/proto 가 밖에서 주입
// =============================================================================

#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/error.h"
#include "atlas/core/ids.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/net/net_types.h"

namespace atlas
{

// BytesHandler 에 오는 것은 소켓이 뱉은 그대로의 바이트
// 메시지도 아니고 무언가 하나의 전체라는 보장도 없다
// 의존 방향은 proto -> net 한쪽뿐. net 은 proto 를 모른다
// [AD 9.1] 동시성은 strand 뿐. 락을 넣으면 교착이 그 교차점에 생긴다
class Session : public std::enable_shared_from_this< Session >
{
public:
    // 세션 자신을 콜백에 넘긴다
    // 소유자가 raw 포인터 -> shared_ptr 맵 없이 같은 연결로 응답 가능
    using BytesHandler =
        std::function< void( const std::shared_ptr< Session >&, std::span< const Byte > ) >;
    using CloseHandler = std::function< void( const std::shared_ptr< Session >& ) >;

    // [AD 9] 백프레셔 상한. 세션은 메시지가 뭔지 여전히 모른다
    // 빼가지 않는 피어를 위해 몇 바이트까지 들고 있을지만 안다
    // 초과 시 폐기가 아니라 연결 종료
    // 조용한 폐기는 며칠 뒤 딴 데서 터진다
    static constexpr std::size_t kMaxWriteQueueBytes = std::size_t{ 1024 } * 1024;

    // [AD 9.3] 전원이 끊긴 피어는 FIN 을 보내지 않는다
    // 타이머가 없으면 그 세션과 버퍼는 영원히 남는다
    // 이 계층의 유일한 시계로 READ 쪽 침묵을 센다
    // 300s 는 데모 제약 - console_client 는 사람이 수십 초를 쉬어간다
    static constexpr Duration kDefaultIdleTimeout = Seconds{ 300 };

    // [CS 4.4] 세션은 반드시 shared_ptr 소유
    // 핸들러마다의 shared_from_this() 가 진행 중 수명을 붙든다
    // 생성자가 private 라 스택 할당은 컴파일 에러
    // idle_timeout 이 인자인 이유는 테스트가 300s 를 못 기다리기 때문
    static std::shared_ptr< Session > Create( IoContext& io_context, Socket socket, SessionId id,
                                              Duration idle_timeout = kDefaultIdleTimeout );

    Session( const Session& ) = delete;
    Session& operator=( const Session& ) = delete;
    Session( Session&& ) = delete;
    Session& operator=( Session&& ) = delete;
    ~Session();

    // 콜백은 Start() 전에 설치할 것
    // 이후 멤버는 strand 소유라 밖에서의 setter 호출은 데이터 경쟁
    void SetBytesHandler( BytesHandler handler );
    void SetCloseHandler( CloseHandler handler );

    void Start();  // 읽기 루프 시작. strand 에 post 하므로 어느 스레드에서나 안전

    // 전송 큐에 적재. 어느 스레드에서나 안전
    // 반환 전에 버퍼를 복사하므로 호출자는 임시 객체를 넘겨도 된다
    void Send( std::span< const Byte > bytes );

    void Close();  // 어느 스레드에서나 안전하고 멱등

    [[nodiscard]] SessionId Id() const noexcept { return id_; }

    // 이 연결의 모든 핸들러가 도는 strand
    // 소유자가 자기 작업을 여기 post 하면 같은 직렬화를 락 없이 물려받는다
    [[nodiscard]] Strand& GetStrand() noexcept { return strand_; }

private:
    Session( IoContext& io_context, Socket socket, SessionId id, Duration idle_timeout );

    // 아래는 전부 strand 위에서 실행
    void StartReadOnStrand();
    void OnReadOnStrand( const ErrorCode& ec, std::size_t bytes_read );
    void ArmIdleTimerOnStrand();  // Start() 와 매 읽기 완료마다 재무장
    void OnIdleTimeoutOnStrand( const ErrorCode& ec );
    void EnqueueOnStrand( std::vector< Byte >&& payload );
    void StartWriteOnStrand();
    void OnWriteOnStrand( const ErrorCode& ec );
    void CloseOnStrand( std::string_view reason );

    // core/error.h 가 주입점으로 남긴 "연결을 안전하게 닫기" 콜백
    // Guarded 가 throw 를 삼켰을 때 연결을 내리는 것이 이것
    [[nodiscard]] FailureHandler MakeFailureHandler();

    // 소켓 읽기 1회분 버퍼. 프로토콜 결정이 아니라 커널이 완료당 넘길 양
    static constexpr std::size_t kReadBufferSize = 4096;

    Socket socket_;
    Strand strand_;
    SessionId id_;
    Ctx ctx_;

    // 다른 멤버와 같이 strand 위에서만 접근. 타이머가 붙은 뒤에도 락은 0개
    SteadyTimer idle_timer_;
    Duration idle_timeout_;

    std::array< Byte, kReadBufferSize > read_buffer_{};

    // [AD 9] 쓰기 정책: FIFO. 진행 중 async_write 는 항상 1개
    // 다음 것은 이전 완료에서 시작
    // strand 덕에 무락 컨테이너와 평범한 누계 멤버가 성립
    std::deque< std::vector< Byte > > write_queue_;
    // write_queue_ 크기 합. 매 Send 마다 deque 를 훑으면 검사 자체가 비용이 됨
    std::size_t write_queue_bytes_{ 0 };

    BytesHandler bytes_handler_;
    CloseHandler close_handler_;
    bool closed_{ false };
};

}  // namespace atlas
