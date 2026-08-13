#pragma once
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

namespace atlas {

// What became of a submitted job.
//
// 🔴 THREE ANSWERS, NOT A BOOL. `false` used to mean only "the runner is stopped"; overload is a
// second, unrelated failure, and folding it into the same `false` would leave the caller unable to
// tell a shutdown from a server under load — the two want opposite responses. This is the same
// overlap `RedisResult::ok` carried until it was taken apart (architecture-design.md §10.6), and it
// is cheaper to not create it than to remove it later.
//
// 🔴 This type is internal to atlas/db. It is NOT a wire contract, no generator produces it, and it
// must never acquire a numeric meaning the client is told about — the game layer maps it onto the
// refusal codes it already has.
enum class SubmitResult : UInt8 {
    // Queued. Exactly one of `work` then `completion` will run.
    Accepted,
    // The queue was at its cap. 🔴 NOTHING runs: not the work, not the completion. The caller
    // already holds the outcome in this return value and answers on the spot, which is the only
    // shape in which a refusal cannot be lost.
    Rejected,
    // Start() was never called, or Stop() already ran. Same "nothing runs" rule as Rejected.
    Stopped,
};

// architecture-design.md §9 — the DB thread pool, the "blocking isolation" branch of the thread
// model diagram.
//
//     I/O threads (io_context)  ->  session strand  ->  DB thread pool  ->  back to that strand
//
// 🔴 These threads are NOT io_context workers and must never be. libmariadb's C API is synchronous:
// a query blocks the thread that issued it. Running one on an I/O worker freezes every session that
// worker was serving, and the symptom is a server that is half alive with no error anywhere.
// Isolation is the entire reason this pool exists — sharing the io_context to save M threads gives
// back the property the design was built around.
//
// 🔴 The ctx ledger crosses the boundary BY VALUE (§9.2). Submit takes a Ctx, not a reference: a
// reference to the caller's CtxScope would dangle the moment that scope unwound on the I/O thread,
// which is the concrete shape of the trap §9.2 warns about. The job re-installs the ledger on the
// DB thread (via Guarded) and the completion re-installs it again wherever it lands.
class DbRunner {
public:
    // Runs on a DB thread with the ctx ledger installed and a leased connection.
    //
    // 🔴 The Connection* is null when no connection could be leased within the acquire timeout. It
    // is a non-owning observer that may be absent (cpp-style.md §4.4), and the job still runs so
    // that the caller always gets exactly one outcome instead of silence.
    //
    // The Ctx& is the job's own copy, and it is the one the transaction scope writes tx_state into
    // (see transaction.h). The completion receives it afterwards.
    using Work = std::function<void(Ctx&, Connection*)>;

    // Runs after the job, on whatever executor the poster sends it to. May be empty.
    using Completion = std::function<void(const Ctx&)>;

    // How to get back to the caller's executor — typically
    // `[strand](std::function<void()> fn) { asio::post(strand, std::move(fn)); }`.
    //
    // 🔴 Type-erased rather than a template parameter so that atlas/db carries no asio dependency
    // at all (cpp-style.md §6 — take the type erasure when the runtime cost is one indirect call
    // and the compile-time cost would be a whole instantiation per executor). Empty means "run the
    // completion inline on the DB thread".
    using CompletionPoster = std::function<void(std::function<void()>)>;

    // How many jobs may WAIT per DB thread before Submit starts refusing.
    //
    // 🔴 The question this number answers is not "how much may pile up" but "can what piled up be
    // drained before the client stops caring". Memory is not the constraint — a few hundred jobs is
    // nothing. Latency is: a queued job is pure waiting that happens BEFORE the request starts, and
    // it stacks on top of the pool acquire wait and the commit fsync that follow it.
    //
    // The settled regime of architecture-design.md §16.1d is ~130 req/s at `db_threads = 2`, so one
    // DB thread drains ~65 jobs/s. 64 per thread is therefore about ONE SECOND of drain. A job that
    // has not even started a second after it arrived will answer a client that has already decided
    // the server is broken, and committing it spends the scarcest resource in the process — a DB
    // thread — on work nobody is waiting for any more.
    //
    // 🔴 Per THREAD rather than a flat total, because §16.1d's ceiling is itself proportional to
    // `db_threads` (`db_threads / (fsyncs per commit x fsync latency)`). A flat cap would silently
    // become a different number of seconds the moment that value is raised.
    static constexpr std::size_t kMaxQueuedJobsPerThread = 64;

    // `thread_count == 0` falls back to 1. Fixed at construction — the pool never resizes.
    DbRunner(ConnectionPool& pool, std::size_t thread_count, Duration acquire_timeout);

    ~DbRunner();

    DbRunner(const DbRunner&) = delete;
    DbRunner& operator=(const DbRunner&) = delete;
    DbRunner(DbRunner&&) = delete;
    DbRunner& operator=(DbRunner&&) = delete;

    void Start();

    // Stops accepting new jobs, drains the ones already queued, then joins. Idempotent.
    // 🔴 Never call from a DB thread — it would join itself.
    void Stop() noexcept;

    [[nodiscard]] std::size_t ThreadCount() const noexcept { return thread_count_; }
    [[nodiscard]] std::size_t QueueCapacity() const noexcept { return queue_capacity_; }
    [[nodiscard]] std::size_t PendingCount() const;

    // ── Server-side counters (architecture-design.md §16.1) ──────────────────────────────────
    // 🔴 Cumulative and monotonic, read with no lock, and NOT logged from here: the caller samples
    // them on an interval of its own. A counter that logged itself per event would be the next
    // bottleneck under exactly the load it exists to describe (§16.1g).
    //
    // The two acquire counters are taken at this call site rather than inside ConnectionPool, and
    // that is exact rather than convenient: §16.1e establishes that DB threads are the pool's only
    // lessees, so every lease in the process passes through the line below.
    [[nodiscard]] UInt64 RejectedCount() const noexcept {
        return rejected_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] UInt64 AcquireFailureCount() const noexcept {
        return acquire_failure_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] UInt64 AcquireWaitMicros() const noexcept {
        return acquire_wait_micros_.load(std::memory_order_relaxed);
    }

    // Queues a job — see SubmitResult for what each answer means and what does NOT run.
    //
    // 🔴 Both refusals are EXPECTED failures, so neither throws (architecture-design.md §11.2a):
    // a stopped runner is shutdown and a full queue is load, and neither is a defect. [[nodiscard]]
    // because dropping the answer is how a refusal turns into a client that waits forever.
    [[nodiscard]] SubmitResult Submit(Ctx ctx, Work work, Completion completion = {},
                                      CompletionPoster poster = {});

private:
    struct Job {
        Ctx ctx{};
        Work work{};
        Completion completion{};
        CompletionPoster poster{};
    };

    void WorkerLoop();
    void RunJob(Job job) noexcept;

    ConnectionPool* pool_;
    std::size_t thread_count_;
    std::size_t queue_capacity_;
    Duration acquire_timeout_;
    std::vector<std::thread> threads_;
    // 🔴 architecture-design.md §9.1 — the same justification the connection pool gives: a job
    // queue shared by N threads is a genuinely shared resource, not strand-serialised session
    // state.
    mutable std::mutex mutex_;
    std::condition_variable pending_;
    std::deque<Job> queue_;
    bool running_{false};

    // 🔴 std::atomic rather than a fourth lock. These are read from a thread that holds nothing and
    // written from threads that hold the queue mutex or nothing at all; a mutex here would add a
    // synchronisation object whose only job is counting, and §9.1's "locks live on genuinely shared
    // resources" is a budget, not a formality.
    std::atomic<UInt64> rejected_count_{0};
    std::atomic<UInt64> acquire_failure_count_{0};
    std::atomic<UInt64> acquire_wait_micros_{0};
};

}  // namespace atlas
