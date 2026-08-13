#include "atlas/db/db_runner.h"

#include <chrono>
#include <optional>
#include <utility>

#include "atlas/core/error.h"
#include "atlas/core/log.h"

namespace atlas {

DbRunner::DbRunner(ConnectionPool& pool, std::size_t thread_count, Duration acquire_timeout)
    : pool_(&pool),
      thread_count_(thread_count == 0 ? 1 : thread_count),
      queue_capacity_(thread_count_ * kMaxQueuedJobsPerThread),
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
    ATLAS_LOG_INFO("db runner started: {} threads, queue cap {}", thread_count_, queue_capacity_);
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

SubmitResult DbRunner::Submit(Ctx ctx, Work work, Completion completion, CompletionPoster poster) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return SubmitResult::Stopped;
        }
        // 🔴 THE LOAD SHEDDING POINT. Past the cap the job is refused instead of queued, and the
        // refusal is silent here on purpose: at overload a refusal is the steady state, so a line
        // per refusal would make the log the next bottleneck (§16.1g). rejected_count_ is what
        // records it, and the caller reports it to its own client.
        if (queue_.size() >= queue_capacity_) {
            rejected_count_.fetch_add(1, std::memory_order_relaxed);
            return SubmitResult::Rejected;
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
    return SubmitResult::Accepted;
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
        // §16.1e — DB threads are the pool's only lessees, so timing the lease HERE measures every
        // lease in the process. Two clock reads against a call that ends in an fsync is noise.
        const TimePoint lease_started = Clock::now();
        std::optional<PooledConnection> lease = pool_->Acquire(acquire_timeout_);
        acquire_wait_micros_.fetch_add(
            static_cast<UInt64>(
                std::chrono::duration_cast<Micros>(Clock::now() - lease_started).count()),
            std::memory_order_relaxed);
        if (!lease.has_value()) {
            acquire_failure_count_.fetch_add(1, std::memory_order_relaxed);
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
