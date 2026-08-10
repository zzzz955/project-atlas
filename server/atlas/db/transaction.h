#pragma once
#include <functional>

#include "atlas/core/ctx.h"
#include "atlas/db/connection.h"

namespace atlas {

// architecture-design.md §10 — the transaction scope is RAII: a scope that unwinds without a commit
// rolls back. 🔴 §10 also says the RAII scope and the exception guard are one set — atomicity only
// holds if a throw inside a Guarded handler unwinds this destructor.
//
// 🔴 The ctx reference is the CALLER'S ledger, deliberately not the thread_local one that CtxScope
// installed. Guarded installs a COPY on entry and restores the previous value on exit (§9.2), so
// anything this scope wrote into the thread_local would be gone by the time the guard returned —
// and "was a transaction open when the guard swallowed that exception?" is precisely the question
// tx_state exists to answer AFTER the guard has returned. Writing through the caller's own Ctx is
// what makes the answer survive. The two are the same values; only the lifetime differs.
class Transaction {
public:
    // Opens the transaction immediately and sets ctx.tx_state to Active. Throws DbException if the
    // server refuses to start one.
    Transaction(Connection& connection, Ctx& ctx);

    // Rolls back and sets ctx.tx_state to RolledBack unless Commit() already ran.
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    // Commits and sets ctx.tx_state to Committed. Throws DbException on failure, in which case the
    // destructor no longer rolls back — the server already ended the transaction.
    void Commit();

    [[nodiscard]] bool IsOpen() const noexcept { return open_; }

private:
    // Non-owning observers: both outlive this scope by construction (cpp-style.md §4.4).
    Connection* connection_;
    Ctx* ctx_;
    bool open_{false};
};

// architecture-design.md §10.4 — compensation. The mirror of Transaction, one step later in time.
//
// 🔴 ROLLBACK AND COMPENSATION ARE NOT TWO NAMES FOR THE SAME THING, AND THE LINE BETWEEN THEM IS
// THE COMMIT. Before the commit, ~Transaction undoes the work and the database never admitted it
// happened. After the commit there is nothing left to roll back: the only way back is a NEW
// committed transaction performing the inverse write, and every reader that saw the intermediate
// state has already seen it. 🔴 Past that line, compensation is the only answer — this scope exists
// so the distinction is structural instead of remembered.
//
//     Transaction tx(connection, ctx);
//     ... writes ...
//     tx.Commit();
//     PostCommitGuard undo(ctx, [&] { ...inverse write, in its own transaction... });
//     PublishTheSideEffect();   // throws -> the guard unwinds -> the inverse write runs
//     undo.Release();
//
// 🔴 What compensation cannot give back is the window. Between the commit and the inverse write the
// committed state is visible to everyone, so a compensated request is not an atomic non-event — it
// is two facts in the log. Saying otherwise would be claiming a distributed transaction, which this
// is not.
class PostCommitGuard {
public:
    // The inverse write. 🔴 Keep it the simplest statement that undoes the commit: it may have to
    // run while an exception is already in flight, and see the destructor note below for what
    // happens when it fails.
    using Compensation = std::function<void()>;

    explicit PostCommitGuard(Compensation compensation);

    // Runs the compensation unless Release() ran first.
    //
    // 🔴 A throwing compensation is caught and logged, because a destructor that throws during an
    // unwind terminates the process. The consequence is stated rather than softened: at that point
    // the committed state really is inconsistent and that log line is the only record of it. That
    // is the reason the compensation must be one inverse write and not a workflow — the more it
    // can do, the more ways it has to fail here, where nothing can be done about it.
    ~PostCommitGuard();

    PostCommitGuard(const PostCommitGuard&) = delete;
    PostCommitGuard& operator=(const PostCommitGuard&) = delete;
    PostCommitGuard(PostCommitGuard&&) = delete;
    PostCommitGuard& operator=(PostCommitGuard&&) = delete;

    // The post-commit step succeeded; there is nothing to undo.
    void Release() noexcept { armed_ = false; }

    [[nodiscard]] bool IsArmed() const noexcept { return armed_; }

private:
    Compensation compensation_;
    bool armed_{true};
};

}  // namespace atlas
