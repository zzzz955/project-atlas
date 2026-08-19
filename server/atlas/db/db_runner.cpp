// =============================================================================
// DbRunner 구현. 부하 차단 지점과 DB 스레드를 지키는 두 겹의 Guarded
// =============================================================================

#include "atlas/db/db_runner.h"

#include <chrono>
#include <optional>
#include <utility>

#include "atlas/core/error.h"
#include "atlas/core/log.h"

namespace atlas
{

DbRunner::DbRunner( ConnectionPool& pool, std::size_t thread_count, Duration acquire_timeout )
    : pool_( &pool ),
      thread_count_( thread_count == 0 ? 1 : thread_count ),
      queue_capacity_( thread_count_ * kMaxQueuedJobsPerThread ),
      acquire_timeout_( acquire_timeout )
{
}

DbRunner::~DbRunner() { Stop(); }

void DbRunner::Start()
{
    {
        const std::lock_guard< std::mutex > lock( mutex_ );
        if ( running_ )
        {
            return;
        }
        running_ = true;
    }
    threads_.reserve( thread_count_ );
    for ( std::size_t index = 0; index < thread_count_; ++index )
    {
        threads_.emplace_back( [this] { WorkerLoop(); } );
    }
    ATLAS_LOG_INFO( "db runner started: {} threads, queue cap {}", thread_count_, queue_capacity_ );
}

void DbRunner::Stop() noexcept
{
    {
        const std::lock_guard< std::mutex > lock( mutex_ );
        if ( !running_ )
        {
            return;
        }
        running_ = false;
    }
    // 큐에 있던 작업은 계속 실행됨
    // 워커는 큐가 비고 running_ 이 false 일 때만 돌아감
    // 여기서 버리면 호출자가 접수됐다고 들은 쓰기를 내버리는 것
    pending_.notify_all();
    for ( std::thread& worker : threads_ )
    {
        if ( worker.joinable() )
        {
            worker.join();
        }
    }
    threads_.clear();
}

std::size_t DbRunner::PendingCount() const
{
    const std::lock_guard< std::mutex > lock( mutex_ );
    return queue_.size();
}

SubmitResult DbRunner::Submit( Ctx ctx, Work work, Completion completion, CompletionPoster poster )
{
    {
        const std::lock_guard< std::mutex > lock( mutex_ );
        if ( !running_ )
        {
            return SubmitResult::Stopped;
        }
        // [AD 16.1g] 부하 차단 지점. 상한을 넘으면 큐에 넣지 않고 거부
        // 여기서 로그를 남기지 않는 것은 일부러
        // 과부하에서는 거부가 정상 상태라 건당 한 줄이 로그를 다음 병목으로 만듦
        // 기록은 rejected_count_ 가 함
        if ( queue_.size() >= queue_capacity_ )
        {
            rejected_count_.fetch_add( 1, std::memory_order_relaxed );
            return SubmitResult::Rejected;
        }
        // [AD 9.2] ctx 는 이 경계에서 작업으로 복사됨
        // 이 줄 아래는 전부 다른 스레드에서 돌고 호출자 프레임을 되짚으면 안 됨
        // Ctx 는 trivially copyable 이라 move 가 아니라 복사
        // std::move 는 없는 소유권 이전을 읽히는 죽은 문법
        Job job{ .ctx = ctx,
                 .work = std::move( work ),
                 .completion = std::move( completion ),
                 .poster = std::move( poster ) };
        queue_.push_back( std::move( job ) );
    }
    pending_.notify_one();
    return SubmitResult::Accepted;
}

void DbRunner::WorkerLoop()
{
    while ( true )
    {
        Job job;
        {
            std::unique_lock< std::mutex > lock( mutex_ );
            pending_.wait( lock, [this] { return !running_ || !queue_.empty(); } );
            if ( queue_.empty() )
            {
                return;
            }
            job = std::move( queue_.front() );
            queue_.pop_front();
        }
        RunJob( std::move( job ) );
    }
}

void DbRunner::RunJob( Job job ) noexcept
{
    // 작업이 바꾸는 원장. 경계를 넘어온 사본에서 출발
    // 트랜잭션 스코프가 tx_state 를 쓰고 completion 이 그것을 읽어감
    Ctx job_ctx = job.ctx;

    // [AD 11.2b] DB 스레드도 같은 async 핸들러 진입점이고 이 풀은 고정 크기
    // 예외 하나가 빠져나가면 스레드가 영구히 빠지고 다시 자라지 않음
    // 큐는 이후 영원히 더 느리게 빠짐
    // Guarded 는 ctx 원장도 심어 trace_id 를 이음
    Guarded(
        job.ctx,
        [this, &job, &job_ctx]
        {
            // [AD 16.1e] DB 스레드가 풀의 유일한 임차인이라 여기서 재면 전부 잼
            // fsync 로 끝나는 호출에 시계 두 번은 노이즈
            const TimePoint lease_started = Clock::now();
            std::optional< PooledConnection > lease = pool_->Acquire( acquire_timeout_ );
            acquire_wait_micros_.fetch_add(
                static_cast< UInt64 >(
                    std::chrono::duration_cast< Micros >( Clock::now() - lease_started ).count() ),
                std::memory_order_relaxed );
            if ( !lease.has_value() )
            {
                acquire_failure_count_.fetch_add( 1, std::memory_order_relaxed );
                ATLAS_LOG_ERROR( "db job ran without a connection: pool exhausted" );
                job.work( job_ctx, nullptr );
                return;
            }
            job.work( job_ctx, &( **lease ) );
        } )();
    // 여기 도달할 때 대여는 이미 반납됨
    // 아래 completion 이 호출자 executor 로 건너뛰는 동안 연결을 잡지 않음

    if ( !job.completion )
    {
        return;
    }

    // [AD 11.2b] 다시 Guarded, 그리고 작업이 남긴 원장으로
    // completion 은 호출자 strand 에서 돌고 거기서의 throw 는 I/O 스레드를 죽임
    // 바깥 가드는 completion 뿐 아니라 넘김 자체도 덮음
    // std::function 생성과 poster(asio::post) 호출이 둘 다 던질 수 있음
    // 죽은 DB 스레드는 다시 살아나지 않으므로 이 함수는 noexcept
    Guarded( job_ctx,
             [&job, &job_ctx]
             {
                 Completion completion = std::move( job.completion );
                 std::function< void() > invoke = [job_ctx, completion]
                 { Guarded( job_ctx, [&completion, &job_ctx] { completion( job_ctx ); } )(); };
                 if ( job.poster )
                 {
                     job.poster( std::move( invoke ) );
                 }
                 else
                 {
                     invoke();
                 }
             } )();
}

}  // namespace atlas
