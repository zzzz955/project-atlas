#include "atlas/db/db_runner.h"

#include <optional>
#include <utility>

#include "atlas/core/error.h"
#include "atlas/core/log.h"

namespace atlas {

DbRunner::DbRunner(ConnectionPool& pool, std::size_t thread_count, Duration acquire_timeout)
    : pool_(&pool),
      thread_count_(thread_count == 0 ? 1 : thread_count),
      acquire_timeout_(acquire_timeout) {}

DbRunner::~DbRunner() { Stop(); }

void DbRunner::Start() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return;
        }
        running_ = true;
    }
    threads_.reserve(thread_count_);
    for (std::size_t index = 0; index < thread_count_; ++index) {
        threads_.emplace_back([this] { WorkerLoop(); });
    }
    ATLAS_LOG_INFO("db runner started: {} threads", thread_count_);
}

void DbRunner::Stop() noexcept {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    // Queued jobs still run: a worker only returns once the queue is empty AND running_ is false.
    // Dropping them here would abandon writes the caller was told had been accepted.
    pending_.notify_all();
    for (std::thread& worker : threads_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    threads_.clear();
}

std::size_t DbRunner::PendingCount() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

bool DbRunner::Submit(Ctx ctx, Work work, Completion completion, CompletionPoster poster) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return false;
        }
        // 🔴 The ctx is copied into the job here, at the boundary. Everything past this line runs
        // on another thread and must not reach back into the caller's frame (§9.2).
        // 🔴 Ctx is trivially copyable, so it is copied — not moved. A std::move() here would be
        // dead syntax that reads as a transfer of ownership the type does not have.
        Job job{.ctx = ctx,
                .work = std::move(work),
                .completion = std::move(completion),
                .poster = std::move(poster)};
        queue_.push_back(std::move(job));
    }
    pending_.notify_one();
    return true;
}

void DbRunner::WorkerLoop() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            pending_.wait(lock, [this] { return !running_ || !queue_.empty(); });
            if (queue_.empty()) {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        RunJob(std::move(job));
    }
}

void DbRunner::RunJob(Job job) noexcept {
    // The ledger the job mutates. Starts as the copy that crossed the boundary; the transaction
    // scope writes tx_state into it and the completion reads it back out.
    Ctx job_ctx = job.ctx;

    // 🔴 architecture-design.md §11.2(b) — a DB thread is an async handler entry point like any
    // other, and this pool is FIXED size: one escaped exception here permanently removes a thread
    // from a pool that never grows one back, and the queue silently drains slower forever after.
    // Guarded also installs the ctx ledger, which is what makes the log macros on a DB thread carry
    // the same trace_id as the I/O thread that submitted the job.
    Guarded(job.ctx, [this, &job, &job_ctx] {
        std::optional<PooledConnection> lease = pool_->Acquire(acquire_timeout_);
        if (!lease.has_value()) {
            ATLAS_LOG_ERROR("db job ran without a connection: pool exhausted");
            job.work(job_ctx, nullptr);
            return;
        }
        job.work(job_ctx, &(**lease));
    })();
    // The lease is released by the time control reaches here, so the completion below never holds
    // a connection hostage while it hops back to the caller's executor.

    if (!job.completion) {
        return;
    }

    // 🔴 Guarded again, and with the ledger the job left behind rather than the one it started
    // with: the completion runs on the caller's strand, where a throw would kill an I/O thread
    // (§11.2b). The ctx travels by value once more — the DB thread's copy is gone by then.
    //
    // The outer guard covers the HAND-OFF as well as the completion: building the std::function and
    // calling into the caller's poster (asio::post) can both throw, and this function is noexcept
    // because a DB thread that dies never comes back.
    Guarded(job_ctx, [&job, &job_ctx] {
        Completion completion = std::move(job.completion);
        std::function<void()> invoke = [job_ctx, completion] {
            Guarded(job_ctx, [&completion, &job_ctx] { completion(job_ctx); })();
        };
        if (job.poster) {
            job.poster(std::move(invoke));
        } else {
            invoke();
        }
    })();
}

}  // namespace atlas
