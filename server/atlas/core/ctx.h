#pragma once
#include "atlas/core/ids.h"
#include "atlas/core/types.h"

namespace atlas {

// architecture-design.md §10 — the transaction scope is RAII and rolls back when the guard unwinds.
// The ledger carries the state so that a `Guarded` failure can tell "nothing was open" apart from
// "a transaction was open and must be rolled back".
enum class TxState : UInt8 { None, Active, Committed, RolledBack };

// architecture-design.md §9.2 — the ctx ledger.
//
// Built at the request entry point and carried across every thread boundary unchanged. Everything
// that has to appear in a log line or an exception message without being threaded through every
// signature lives here.
//
// `trace_id` is intentionally a plain `UInt64` rather than a strong-typed ID: it is generated and
// compared, never passed alongside another identifier of the same shape, so the swap-arguments
// mistake the strong types exist to prevent cannot happen to it.
struct Ctx {
    UInt64 trace_id{0};
    // Value-initialised rather than `{0}`: MSVC rejects the integer literal in a default member
    // initialiser for a scoped enumeration, and `{}` says "unset" more directly anyway.
    AccountId account_id{};
    CharacterId character_id{};
    SessionId session_id{};
    TxState tx_state{TxState::None};
};

// The ledger installed on the calling thread right now. Never null; an unset ledger is a
// default-constructed `Ctx`.
const Ctx& CurrentCtx() noexcept;

// 🔴 architecture-design.md §9.2 — a plain `thread_local` is NOT enough.
//
// A strand serialises handlers but does not pin them to a thread, so the executing thread changes
// from handler to handler. The storage may be thread_local, but correctness comes from installing
// on entry and restoring on exit — which is exactly what this RAII scope does. `Guarded`
// (core/error.h) is the handler-boundary user of it; nesting works because each scope restores the
// exact value it displaced.
class CtxScope {
public:
    explicit CtxScope(const Ctx& ctx) noexcept;
    ~CtxScope();

    CtxScope(const CtxScope&) = delete;
    CtxScope& operator=(const CtxScope&) = delete;
    CtxScope(CtxScope&&) = delete;
    CtxScope& operator=(CtxScope&&) = delete;

private:
    Ctx previous_;
};

}  // namespace atlas
