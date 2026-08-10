#pragma once
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "atlas/core/ctx.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"

// architecture-design.md §10.4 — at-most-once handling of a request that crossed a boundary.
//
// 🔴 THE PROBLEM THIS SOLVES CANNOT BE SOLVED BY LOOKING HARDER AT THE REQUEST.
// "A retransmit of a request whose response was lost" and "a genuinely new duplicate request" are
// indistinguishable at the receiver. Nothing in the bytes separates them, no timeout makes the
// difference observable, and a sender that gave up waiting cannot know which side of the boundary
// its request died on. A sender-generated idempotency key is the only construction that faces that
// instead of hoping around it: the sender labels the request, and the receiver promises to execute
// a label at most once and to hand back the recorded result for every repeat of it.
//
// 🔴 IN MEMORY, AND THAT IS A LIMITATION RATHER THAN A DETAIL.
// In production the record belongs in a database table, so that it survives the process that made
// it. Here it is a map: the demo schema's table budget is spent (§3.3), and inventing a table to
// make a test look better would be pretending to a property this code does not have. The honest
// consequence, stated once here and asserted in tests/tx_atomicity_test.cpp:
//
//     A restart loses every record in this store, so the at-most-once promise does NOT survive a
//     crash on its own. What survives a crash is the COMMITTED DATABASE STATE. Recovery therefore
//     has to be driven from durable state and not from this map — that is what Seed() is for, and
//     a request whose effect leaves no durable trace cannot be recovered at all.
//
// 🔴 What is implemented here is idempotency and recoverability across a boundary. It is NOT a
// distributed transaction: there is no 2PC, no Saga coordinator, no atomicity across two databases,
// and no handling of a network partition.

namespace atlas {

// What Begin() decided for this caller.
enum class RequestAdmission : UInt8 {
    // This caller now owns the key. Execute the request.
    Started,
    // The key already completed. Use the recorded result; 🔴 do not execute.
    Replay,
    // Another caller owns the key and had not finished within the wait budget. Nothing was
    // executed, and there is no result yet.
    InProgress,
};

struct RequestAdmissionResult {
    RequestAdmission admission{RequestAdmission::Started};
    RequestState state{RequestState::None};
    // Non-empty only for Replay: the result blob recorded by the execution that did happen. Opaque
    // here — this layer never interprets it, it only guarantees the same bytes come back.
    std::string result{};
};

// Keyed record of in-flight and completed requests, with a TTL.
//
// 🔴 Thread-safe, and that is the whole point of it: the state transition has to be atomic or two
// simultaneous arrivals of the same key both execute. The lock is correct here for the reason
// §9.1 gives for the connection pool — this is a genuinely shared resource contended by unrelated
// threads, not strand-serialised session state.
class IdempotencyStore {
public:
    // 🔴 `entry_ttl` must exceed the longest a request can take. It is a memory bound on completed
    // records, not a timeout on execution: expiring a record while its request is still running
    // would let a retransmit start a second execution, which is the failure this class exists to
    // prevent.
    explicit IdempotencyStore(Duration entry_ttl);

    // Claims `ctx.request_key` for execution, or reports what is already known about it.
    //
    // When the key is in flight elsewhere, waits up to `wait_budget` for that execution to reach
    // Persisted and then replays its result — so a retransmit arriving mid-execution either waits
    // for the answer or is told the request is in progress, and never executes a second copy.
    //
    // 🔴 The wait is bounded, for the reason ConnectionPool::Acquire gives: an unbounded wait turns
    // one slow request into a stalled server that looks like a hang rather than a fault.
    //
    // Writes the resulting state into `ctx.request_state`.
    RequestAdmissionResult Begin(Ctx& ctx, Duration wait_budget);

    // Records the committed result and releases every waiter on this key.
    //
    // 🔴 THE RESPONSE GOES OUT AFTER THE COMMIT AND NEVER BEFORE. Answering first is always
    // tempting — it shortens the latency the client measures — and it is always wrong: the client
    // then holds a result the database may never accept, and no later apology can retract a number
    // the player already saw. The `Active` check below is that ordering made mechanical rather than
    // remembered; a transaction still open at this point means the caller is about to answer from
    // state the database has not accepted yet.
    void MarkPersisted(Ctx& ctx, std::string result);

    // Records that the client was told. 🔴 Rejects a key that never passed through Persisted —
    // the state machine may be resumed from, never skipped.
    void MarkResponded(Ctx& ctx);

    // Installs a Persisted record for a request THIS PROCESS NEVER EXECUTED, rebuilt from durable
    // state after a restart.
    //
    // 🔴 This is the honest answer to "the store is in memory, so what happens after a crash?".
    // Nothing in this map survives; the committed rows do. Recovery reads the durable evidence of
    // the request — for a write whose primary key is the request key, its mere presence is that
    // evidence — reconstructs the result from it, and seeds the record here so the state machine
    // resumes at Persisted rather than re-executing.
    //
    // 🔴 It resumes; it does not replay. The exact response bytes the client never received are
    // gone with the process, and the reconstruction is only as faithful as the durable state still
    // determines it. Naming that gap is the point of this comment.
    void Seed(Ctx& ctx, std::string result);

    // Forgets the key so that a retransmit executes from scratch. For the two cases where the
    // request did not, in the end, happen: the transaction rolled back, or a post-commit
    // compensation undid it (see PostCommitGuard in db/transaction.h). Waiters are released and
    // re-decide — one of them becomes the new owner.
    void Abandon(Ctx& ctx);

    [[nodiscard]] std::optional<RequestState> Find(RequestKey key) const;

    // Drops expired records. Returns how many went. Nothing calls this on a timer yet — the owner
    // of the timer is the request layer, which does not exist; leaving the sweep callable and
    // uncalled is visible, whereas a background thread nobody asked for is not.
    std::size_t PurgeExpired();

    [[nodiscard]] std::size_t Size() const;

private:
    struct Entry {
        RequestState state{RequestState::Received};
        std::string result{};
        TimePoint expires_at{};
    };

    mutable std::mutex mutex_;
    std::condition_variable finished_;
    // Keyed by the raw value: RequestKey is a scoped enumeration and hashing it would need a
    // specialisation whose only user is this map.
    std::unordered_map<UInt64, Entry> entries_;
    Duration entry_ttl_;
};

}  // namespace atlas
