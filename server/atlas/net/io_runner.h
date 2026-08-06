#pragma once
#include <boost/asio/executor_work_guard.hpp>
#include <cstddef>
#include <thread>
#include <vector>

#include "atlas/net/net_types.h"

namespace atlas {

// architecture-design.md §9 — the I/O thread pool at the top of the thread model.
//
// One `io_context`, N worker threads calling `run()` on it. Nothing else: the serialisation of any
// particular connection is the session's own strand (§9.1), not the number of threads here, so this
// class stays a lifetime owner and never grows a dispatch policy.
//
// 🔴 Start / Stop are for the thread that owns the process lifetime; they are not thread-safe
// against each other and Stop must never be called from a worker (it would join itself).
class IoRunner {
public:
    // `worker_count == 0` means `std::thread::hardware_concurrency()`, falling back to 1 when the
    // platform cannot report it. The count is fixed at construction — the pool never resizes.
    explicit IoRunner(std::size_t worker_count = 0);
    ~IoRunner();

    IoRunner(const IoRunner&) = delete;
    IoRunner& operator=(const IoRunner&) = delete;
    IoRunner(IoRunner&&) = delete;
    IoRunner& operator=(IoRunner&&) = delete;

    void Start();

    // Graceful stop, and the ordering matters (architecture-design.md §9):
    //     acceptor.Stop()  ->  session.Close() for every live session  ->  runner.Stop()
    // Releasing the work guard only lets `run()` return once the queue has drained, so a socket
    // still holding a pending read keeps the pool alive. Closing the front door first is what makes
    // the drain finite. Idempotent: calling it twice is a no-op.
    void Stop() noexcept;

    [[nodiscard]] IoContext& Context() noexcept { return io_context_; }
    [[nodiscard]] std::size_t WorkerCount() const noexcept { return worker_count_; }

private:
    void JoinWorkers() noexcept;

    IoContext io_context_;
    // 🔴 Spelled out rather than aliased: cpp-style.md §4.2 admits an alias only for a type that is
    // both verbose and frequent, and this one appears exactly once in the project.
    asio::executor_work_guard<IoContext::executor_type> work_guard_;
    std::vector<std::thread> workers_;
    std::size_t worker_count_;
};

}  // namespace atlas
