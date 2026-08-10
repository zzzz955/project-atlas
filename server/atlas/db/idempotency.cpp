#include "atlas/db/idempotency.h"

#include <utility>

#include "atlas/core/error.h"

namespace atlas {
namespace {

// A key of zero means the request entry point never set one, which would silently turn every
// unlabelled request into the same request.
UInt64 RequireKey(const Ctx& ctx) {
    ATLAS_CHECK(ctx.request_key != RequestKey{},
                "idempotency key is unset; the request entry point must fill ctx.request_key");
    return RequestKeyValue(ctx.request_key);
}

}  // namespace

IdempotencyStore::IdempotencyStore(Duration entry_ttl) : entry_ttl_(entry_ttl) {}

RequestAdmissionResult IdempotencyStore::Begin(Ctx& ctx, Duration wait_budget) {
    const UInt64 key = RequireKey(ctx);
    const TimePoint wait_deadline = Clock::now() + wait_budget;

    std::unique_lock<std::mutex> lock(mutex_);
    bool waited_out = false;
    for (;;) {
        const auto found = entries_.find(key);
        const bool live = found != entries_.end() && Clock::now() < found->second.expires_at;

        if (!live) {
            // 🔴 The lookup and the claim are one critical section. Splitting them — check, unlock,
            // insert — is exactly how two simultaneous arrivals of the same key both become owners,
            // and the resulting double execution is invisible until the numbers stop adding up.
            Entry fresh;
            fresh.state = RequestState::Received;
            fresh.expires_at = Clock::now() + entry_ttl_;
            entries_.insert_or_assign(key, std::move(fresh));
            ctx.request_state = RequestState::Received;
            return {RequestAdmission::Started, RequestState::Received, std::string{}};
        }

        if (found->second.state != RequestState::Received) {
            ctx.request_state = found->second.state;
            return {RequestAdmission::Replay, found->second.state, found->second.result};
        }

        // Owned by someone else and still running. Re-checked once after the budget expires so a
        // completion that landed during the wait is answered with its result rather than with
        // "in progress".
        if (waited_out) {
            ctx.request_state = RequestState::Received;
            return {RequestAdmission::InProgress, RequestState::Received, std::string{}};
        }
        waited_out = finished_.wait_until(lock, wait_deadline) == std::cv_status::timeout;
    }
}

void IdempotencyStore::MarkPersisted(Ctx& ctx, std::string result) {
    // 🔴 The commit-then-respond ordering, made mechanical. See the header: an open transaction
    // here means the caller is one step away from telling the client something the database has not
    // accepted.
    ATLAS_CHECK(ctx.tx_state != TxState::Active,
                "request marked Persisted while its transaction is still open — the response must "
                "never precede the commit");
    const UInt64 key = RequireKey(ctx);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(key);
        ATLAS_CHECK(found != entries_.end(),
                    "request marked Persisted without an admission record; the key expired or "
                    "Begin was never called");
        found->second.state = RequestState::Persisted;
        found->second.result = std::move(result);
        found->second.expires_at = Clock::now() + entry_ttl_;
    }
    ctx.request_state = RequestState::Persisted;
    finished_.notify_all();
}

void IdempotencyStore::MarkResponded(Ctx& ctx) {
    const UInt64 key = RequireKey(ctx);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(key);
        ATLAS_CHECK(found != entries_.end(),
                    "request marked Responded without an admission record");
        ATLAS_CHECK(found->second.state == RequestState::Persisted,
                    "request marked Responded without passing through Persisted");
        found->second.state = RequestState::Responded;
    }
    ctx.request_state = RequestState::Responded;
}

void IdempotencyStore::Seed(Ctx& ctx, std::string result) {
    const UInt64 key = RequireKey(ctx);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry recovered;
        recovered.state = RequestState::Persisted;
        recovered.result = std::move(result);
        recovered.expires_at = Clock::now() + entry_ttl_;
        entries_.insert_or_assign(key, std::move(recovered));
    }
    ctx.request_state = RequestState::Persisted;
    finished_.notify_all();
}

void IdempotencyStore::Abandon(Ctx& ctx) {
    const UInt64 key = RequireKey(ctx);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(key);
    }
    ctx.request_state = RequestState::None;
    // Waiters must wake even though nothing completed: the key is free now and one of them becomes
    // the owner. Leaving them parked until their budget expires would report "in progress" about a
    // request that no longer exists.
    finished_.notify_all();
}

std::optional<RequestState> IdempotencyStore::Find(RequestKey key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(RequestKeyValue(key));
    if (found == entries_.end() || Clock::now() >= found->second.expires_at) {
        return std::nullopt;
    }
    return found->second.state;
}

std::size_t IdempotencyStore::PurgeExpired() {
    const TimePoint now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t before = entries_.size();
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (now >= it->second.expires_at) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    return before - entries_.size();
}

std::size_t IdempotencyStore::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

}  // namespace atlas
