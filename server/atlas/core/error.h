#pragma once
#include <exception>
#include <format>
#include <functional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "atlas/core/ctx.h"
#include "atlas/core/log.h"
#include "atlas/core/types.h"

// architecture-design.md §11.2 — exception policy.
//
// (a) Expected failures are not exceptions. A malformed packet, a missing character, a DB miss —
//     those are `std::expected` / `error_code` / `optional`, following the grain asio already sets
//     with its error_code overloads. Exceptions are for programming errors and unrecoverable state.
//
// ⚠️ 🔴 No asio dependency in this header. `Guarded` wraps an arbitrary callable; it knows nothing
// about sessions or sockets. "Close the session safely" arrives as an injected callback, and the
// actual session wiring happens in the net layer.

namespace atlas {

// The one exception type the core ships. Layers derive their own from it when they need to be
// caught selectively; there is no speculative hierarchy here.
class Exception : public std::runtime_error {
public:
    explicit Exception(const std::string& message,
                       std::source_location where = std::source_location::current());

    [[nodiscard]] const std::source_location& Where() const noexcept { return where_; }
    // The trace id of the ctx ledger that was installed when the exception was constructed, so a
    // caught exception can still be correlated with the request that produced it.
    [[nodiscard]] UInt64 TraceId() const noexcept { return trace_id_; }
    [[nodiscard]] std::string_view StackTrace() const noexcept { return stack_trace_; }

private:
    std::source_location where_;
    UInt64 trace_id_;
    std::string stack_trace_;
};

namespace detail {

void ReportGuardedException(const std::exception& failure) noexcept;
void ReportGuardedUnknownException() noexcept;

}  // namespace detail

// Injected by the handler owner: "the guard swallowed a throw, take this connection down safely".
// A std::function rather than another template parameter — cpp-style.md §6 prefers type erasure
// where the runtime cost is a single indirect call and the compile-time cost is a whole extra
// instantiation of every guarded handler.
using FailureHandler = std::function<void(std::string_view reason)>;

// 🔴 architecture-design.md §11.2(b) — an exception that escapes an async handler propagates to
// io_context::run() and kills that I/O thread. Every session that thread served then goes silent
// while the process stays up: the worst failure shape there is. Every handler entry point is
// therefore wrapped:
//
//     asio::post(strand_, atlas::Guarded(ctx, [self] { ... }));
//
// Three responsibilities: ① install the ctx ledger for the duration (RAII, so the log macros pick
// it up for free) ② catch everything, log ERROR, invoke the failure callback ③ be noexcept.
//
// 🔴 A template wrapper, not a try/catch macro (cpp-style.md §5): a macro would have to get brace
// matching, `return` and nesting right, and the debugger would not step through it.
template <class Handler>
class Guarded {
public:
    Guarded(Ctx ctx, Handler handler) : ctx_(ctx), handler_(std::move(handler)) {}

    Guarded(Ctx ctx, Handler handler, FailureHandler on_failure)
        : ctx_(ctx), handler_(std::move(handler)), on_failure_(std::move(on_failure)) {}

    template <class... Args>
    void operator()(Args&&... args) noexcept {
        const CtxScope scope(ctx_);
        try {
            handler_(std::forward<Args>(args)...);
        } catch (const std::exception& ex) {
            Fail(ex);
        } catch (...) {
            FailUnknown();
        }
    }

private:
    void Fail(const std::exception& failure) noexcept {
        // The guard is the last line of defence, so even the reporting path has to be total: an
        // allocation failure inside the ERROR log must not turn into a terminate().
        try {
            detail::ReportGuardedException(failure);
            if (on_failure_) {
                on_failure_(failure.what());
            }
        } catch (...) {  // NOLINT — nowhere left to report to.
        }
    }

    void FailUnknown() noexcept {
        try {
            detail::ReportGuardedUnknownException();
            if (on_failure_) {
                on_failure_("non-std exception");
            }
        } catch (...) {  // NOLINT — nowhere left to report to.
        }
    }

    Ctx ctx_;
    Handler handler_;
    FailureHandler on_failure_;
};

template <class Handler>
Guarded(Ctx, Handler) -> Guarded<Handler>;

template <class Handler>
Guarded(Ctx, Handler, FailureHandler) -> Guarded<Handler>;

}  // namespace atlas

// cpp-style.md §5 — the whitelist closes here: six log macros plus these three, and nothing else.
// `std::source_location` is captured by the defaulted constructor parameter, evaluated at the
// expansion site, so none of them need __FILE__/__LINE__.

// Throws `Type`, built from a std::format message, with source location and ctx attached.
#define ATLAS_THROW(Type, ...) throw Type(::std::format(__VA_ARGS__))

// Stays alive in release builds: this is for invariants whose violation means the process is
// already in a state it cannot reason about.
#define ATLAS_CHECK(cond, ...)                            \
    do {                                                  \
        if (!(cond)) {                                    \
            ATLAS_THROW(::atlas::Exception, __VA_ARGS__); \
        }                                                 \
    } while (false)

#if defined(NDEBUG)
// 🔴 Compiled out in release, so the condition is never evaluated there — never put a side effect
// inside ATLAS_ASSERT. Use ATLAS_CHECK when the check itself must survive.
#define ATLAS_ASSERT(cond) \
    do {                   \
    } while (false)
#else
#define ATLAS_ASSERT(cond) ATLAS_CHECK(cond, "assertion failed: {}", #cond)
#endif
