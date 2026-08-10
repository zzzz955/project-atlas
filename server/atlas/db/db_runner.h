#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/time.h"
#include "atlas/db/connection.h"
#include "atlas/db/connection_pool.h"

namespace atlas {

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
    [[nodiscard]] std::size_t PendingCount() const;

    // Queues a job. Returns false when the runner is not running, in which case nothing will run —
    // an expected failure at shutdown, so not an exception (architecture-design.md §11.2a).
    bool Submit(Ctx ctx, Work work, Completion completion = {}, CompletionPoster poster = {});

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
    Duration acquire_timeout_;
    std::vector<std::thread> threads_;
    // 🔴 architecture-design.md §9.1 — the same justification the connection pool gives: a job
    // queue shared by N threads is a genuinely shared resource, not strand-serialised session
    // state.
    mutable std::mutex mutex_;
    std::condition_variable pending_;
    std::deque<Job> queue_;
    bool running_{false};
};

}  // namespace atlas
