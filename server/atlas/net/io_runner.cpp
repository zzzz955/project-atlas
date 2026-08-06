#include "atlas/net/io_runner.h"

#include <boost/asio/executor_work_guard.hpp>

#include "atlas/core/error.h"

namespace atlas {

IoRunner::IoRunner(std::size_t worker_count)
    : work_guard_(asio::make_work_guard(io_context_)), worker_count_(worker_count) {
    if (worker_count_ == 0) {
        const unsigned hardware = std::thread::hardware_concurrency();
        // hardware_concurrency() is allowed to answer 0 when it cannot tell; one worker is still a
        // working server, so the default degrades instead of failing.
        worker_count_ = hardware == 0 ? std::size_t{1} : static_cast<std::size_t>(hardware);
    }
}

IoRunner::~IoRunner() {
    // Safety net, not the intended path. A caller that forgot Stop() must not hang the process at
    // teardown, so this abandons whatever is still queued rather than waiting for it to drain —
    // the opposite of what Stop() does, and deliberately so.
    work_guard_.reset();
    if (!workers_.empty()) {
        io_context_.stop();
        JoinWorkers();
    }
}

void IoRunner::Start() {
    ATLAS_CHECK(workers_.empty(), "IoRunner::Start called twice");

    workers_.reserve(worker_count_);
    for (std::size_t i = 0; i < worker_count_; ++i) {
        workers_.emplace_back([this] {
            // 🔴 architecture-design.md §11.2(b) — there is deliberately NO try/catch here. Every
            // handler entry point is already wrapped in `Guarded`, so an exception reaching this
            // frame means a handler is missing its guard. Catching it here would restart the
            // worker and hide exactly the defect the guard exists to prevent; letting it terminate
            // the process makes a missing guard loud instead of silent.
            io_context_.run();
        });
    }
}

void IoRunner::Stop() noexcept {
    work_guard_.reset();
    JoinWorkers();
}

void IoRunner::JoinWorkers() noexcept {
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

}  // namespace atlas
