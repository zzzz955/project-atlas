#pragma once

// =============================================================================
// io_context 1개 + 워커 N개. 스레드 모델 최상단의 I/O 풀
// 연결별 직렬화는 세션 strand 의 몫이라 여기엔 디스패치 정책이 없음
// =============================================================================

#include <boost/asio/executor_work_guard.hpp>
#include <cstddef>
#include <thread>
#include <vector>

#include "atlas/net/net_types.h"

namespace atlas
{

// [AD 9] Start / Stop 은 프로세스 수명을 쥔 스레드 전용
// 둘은 서로 스레드 안전하지 않다
// Stop 을 워커에서 부르면 자기 자신을 join 하게 된다
class IoRunner
{
public:
    // worker_count == 0 이면 hardware_concurrency(), 그것도 0 이면 1
    explicit IoRunner( std::size_t worker_count = 0 );
    ~IoRunner();

    IoRunner( const IoRunner& ) = delete;
    IoRunner& operator=( const IoRunner& ) = delete;
    IoRunner( IoRunner&& ) = delete;
    IoRunner& operator=( IoRunner&& ) = delete;

    void Start();

    // [AD 9] 종료 순서: acceptor.Stop() -> 각 session.Close() -> runner.Stop()
    // work guard 만 풀면 큐가 빌 때까지 run() 이 돌아오지 않는다
    // 대문을 먼저 닫아야 드레인이 유한해진다. 두 번 불러도 무해
    void Stop() noexcept;

    [[nodiscard]] IoContext& Context() noexcept { return io_context_; }
    [[nodiscard]] std::size_t WorkerCount() const noexcept { return worker_count_; }

private:
    void JoinWorkers() noexcept;

    IoContext io_context_;
    // [CS 4.2] 별칭은 장황하고 잦은 타입에만. 이건 프로젝트에 딱 한 번 등장
    asio::executor_work_guard< IoContext::executor_type > work_guard_;
    std::vector< std::thread > workers_;
    std::size_t worker_count_;
};

}  // namespace atlas
