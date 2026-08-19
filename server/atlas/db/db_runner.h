#pragma once

// =============================================================================
// [AD 9] DB 스레드 풀. 스레드 모델의 blocking isolation 가지
// I/O 스레드 -> 세션 strand -> DB 스레드 풀 -> 다시 그 strand
// =============================================================================

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/db/connection_pool.h"

namespace atlas
{

// 제출한 작업의 결말. bool 이 아니라 답이 셋인 이유
// 예전 false 는 "러너가 멈춤" 만 뜻했고 과부하는 무관한 두 번째 실패
// 하나로 접으면 호출자가 종료와 부하를 구분 못 함. 둘의 대응은 정반대
// [AD 10.6] RedisResult::ok 가 분해되기 전까지 지고 있던 것과 같은 겹침
// atlas/db 내부용이라 클라이언트에게 알려지는 숫자 의미를 가져선 안 됨
enum class SubmitResult : UInt8
{
    // 큐에 들어감. work 다음 completion 이 정확히 한 번씩 실행
    Accepted,
    // 큐가 상한. work 도 completion 도 실행되지 않음
    // 호출자가 이 반환값으로 결과를 이미 쥐고 그 자리에서 답함
    // 거부가 유실될 수 없는 유일한 형태
    Rejected,
    // Start() 미호출이거나 Stop() 이 이미 돎. Rejected 와 같은 "아무것도 안 함"
    Stopped,
};

// 이 스레드들은 io_context 워커가 아니며 절대 아니어야 함
// libmariadb 의 C API 는 동기라 쿼리가 발행한 스레드를 막음
// I/O 워커에서 돌리면 그 워커가 맡은 모든 세션이 얼어붙음
// [AD 9.2] ctx 원장은 값으로 경계를 넘음
// 호출자 CtxScope 로의 참조는 그 스코프가 풀리는 순간 매달림
class DbRunner
{
public:
    // ctx 원장이 심긴 DB 스레드에서 대여한 연결과 함께 실행
    // [CS 4.4] Connection* 은 대여 실패 시 null - 없을 수 있는 비소유 관찰자
    // 그래도 작업은 돌아 호출자가 침묵 대신 결말 하나를 받음
    using Work = std::function< void( Ctx&, Connection* ) >;

    // 작업 뒤 poster 가 보낸 executor 위에서 실행. 비어 있어도 됨
    using Completion = std::function< void( const Ctx& ) >;

    // 호출자 executor 로 돌아가는 방법. 보통 strand 에 asio::post 하는 람다
    // [CS 6] 타입 소거인 이유 - atlas/db 가 asio 의존을 전혀 지지 않게 하려고
    // 비면 completion 을 DB 스레드에서 인라인 실행
    using CompletionPoster = std::function< void( std::function< void() > ) >;

    // DB 스레드 1개당 대기 가능한 작업 수. 넘으면 Submit 이 거부하기 시작
    // 제약은 메모리가 아니라 지연. 쌓인 것을 포기 전에 빼낼 수 있어야 함
    // [AD 16.1d] 정착 구간이 db_threads = 2 에서 약 130 req/s
    // 스레드 하나가 초당 약 65개를 빼내므로 스레드당 64 는 약 1초치 배수량
    // 총량이 아니라 스레드당인 이유 - 천장 자체가 db_threads 에 비례
    static constexpr std::size_t kMaxQueuedJobsPerThread = 64;

    // thread_count == 0 이면 1로 대체. 생성 시 고정 - 풀은 리사이즈하지 않음
    DbRunner( ConnectionPool& pool, std::size_t thread_count, Duration acquire_timeout );

    ~DbRunner();

    DbRunner( const DbRunner& ) = delete;
    DbRunner& operator=( const DbRunner& ) = delete;
    DbRunner( DbRunner&& ) = delete;
    DbRunner& operator=( DbRunner&& ) = delete;

    void Start();

    // 새 작업 접수를 멈추고 이미 큐에 있는 것을 빼낸 뒤 join. 멱등
    // DB 스레드에서 부르지 말 것 - 자기 자신을 join 하게 됨
    void Stop() noexcept;

    [[nodiscard]] std::size_t ThreadCount() const noexcept { return thread_count_; }
    [[nodiscard]] std::size_t QueueCapacity() const noexcept { return queue_capacity_; }
    [[nodiscard]] std::size_t PendingCount() const;

    // [AD 16.1] 서버측 카운터. 누적 단조이고 락 없이 읽음
    // 여기서 로그를 남기지 않음 - 표본 주기는 호출자 몫
    // [AD 16.1g] 이벤트마다 로그하는 카운터는 그 부하에서 다음 병목이 됨
    // [AD 16.1e] acquire 카운터 둘을 풀 안이 아니라 호출 지점에서 셈
    // DB 스레드가 풀의 유일한 임차인이라 모든 대여가 아래 줄을 지남
    [[nodiscard]] UInt64 RejectedCount() const noexcept
    {
        return rejected_count_.load( std::memory_order_relaxed );
    }
    [[nodiscard]] UInt64 AcquireFailureCount() const noexcept
    {
        return acquire_failure_count_.load( std::memory_order_relaxed );
    }
    [[nodiscard]] UInt64 AcquireWaitMicros() const noexcept
    {
        return acquire_wait_micros_.load( std::memory_order_relaxed );
    }

    // 작업을 큐에 넣음. 각 답의 뜻은 SubmitResult
    // [AD 11.2a] 두 거부 모두 예상된 실패라 던지지 않음
    // 멈춘 러너는 종료이고 꽉 찬 큐는 부하이며 어느 쪽도 결함이 아님
    // 답을 버리면 거부가 영원히 기다리는 클라이언트가 되므로 [[nodiscard]]
    [[nodiscard]] SubmitResult Submit( Ctx ctx, Work work, Completion completion = {},
                                       CompletionPoster poster = {} );

private:
    struct Job
    {
        Ctx ctx{};
        Work work{};
        Completion completion{};
        CompletionPoster poster{};
    };

    void WorkerLoop();
    void RunJob( Job job ) noexcept;

    ConnectionPool* pool_;
    std::size_t thread_count_;
    std::size_t queue_capacity_;
    Duration acquire_timeout_;
    std::vector< std::thread > threads_;
    // [AD 9.1] 연결 풀과 같은 근거
    // N 개 스레드가 공유하는 작업 큐는 세션 상태가 아니라 진짜 공유 자원
    mutable std::mutex mutex_;
    std::condition_variable pending_;
    std::deque< Job > queue_;
    bool running_{ false };

    // 네 번째 락이 아니라 std::atomic
    // 여기 뮤텍스를 두면 세는 일만 하는 동기화 객체가 하나 늚
    // [AD 9.1] 의 락 예산은 형식이 아님
    std::atomic< UInt64 > rejected_count_{ 0 };
    std::atomic< UInt64 > acquire_failure_count_{ 0 };
    std::atomic< UInt64 > acquire_wait_micros_{ 0 };
};

}  // namespace atlas
