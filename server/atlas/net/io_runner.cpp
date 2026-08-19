// =============================================================================
// IoRunner 구현. 워커 수 결정, 기동, 정지와 조인
// =============================================================================

#include "atlas/net/io_runner.h"

#include <boost/asio/executor_work_guard.hpp>

#include "atlas/core/error.h"

namespace atlas
{

IoRunner::IoRunner( std::size_t worker_count )
    : work_guard_( asio::make_work_guard( io_context_ ) ), worker_count_( worker_count )
{
    if ( worker_count_ == 0 )
    {
        const unsigned hardware = std::thread::hardware_concurrency();
        // 0 을 답해도 됨. 워커 1개여도 서버는 돌아가므로 실패 대신 강등
        worker_count_ = hardware == 0 ? std::size_t{ 1 } : static_cast< std::size_t >( hardware );
    }
}

IoRunner::~IoRunner()
{
    // 안전망일 뿐 정상 경로가 아니다
    // Stop() 을 잊어도 종료가 멈추지 않도록 남은 큐를 드레인 없이 버린다
    // Stop() 과는 정반대이고 의도된 것
    // 소멸자는 noexcept 인데 join 은 throw 하므로 이 catch 가 없으면 std::terminate
    try
    {
        work_guard_.reset();
        if ( !workers_.empty() )
        {
            io_context_.stop();
            JoinWorkers();
        }
    }
    catch ( ... )  // NOLINT - noexcept 종료 경로엔 보고할 곳이 없음
    {
    }
}

void IoRunner::Start()
{
    ATLAS_CHECK( workers_.empty(), "IoRunner::Start called twice" );

    workers_.reserve( worker_count_ );
    for ( std::size_t i = 0; i < worker_count_; ++i )
    {
        workers_.emplace_back(
            [this]
            {
                // [AD 11.2b] try/catch 가 없는 것은 의도
                // 진입점이 이미 Guarded 라 여기까지 온 예외는 가드 누락이다
                // 잡으면 그 결함을 감추게 된다
                io_context_.run();
            } );
    }
}

void IoRunner::Stop() noexcept
{
    work_guard_.reset();
    JoinWorkers();
}

void IoRunner::JoinWorkers() noexcept
{
    for ( std::thread& worker : workers_ )
    {
        if ( worker.joinable() )
        {
            worker.join();
        }
    }
    workers_.clear();
}

}  // namespace atlas
