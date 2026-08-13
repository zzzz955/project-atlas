#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "atlas/core/time.h"
#include "atlas/db/connection.h"

namespace atlas {

class ConnectionPool;

// RAII lease. The connection goes back to the pool when this object dies, on every path including
// an exception unwinding through the frame that held it.
class PooledConnection {
public:
    ~PooledConnection();

    PooledConnection(const PooledConnection&) = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;
    // Movable so that Acquire can return one out of an std::optional. A moved-from lease owns
    // nothing and returns nothing.
    PooledConnection(PooledConnection&& other) noexcept;
    PooledConnection& operator=(PooledConnection&& other) noexcept;

    [[nodiscard]] Connection& operator*() const noexcept { return *connection_; }
    [[nodiscard]] Connection* operator->() const noexcept { return connection_; }

private:
    friend class ConnectionPool;
    PooledConnection(ConnectionPool& pool, Connection& connection) noexcept;

    void Return() noexcept;

    // Non-owning observers, both of them (cpp-style.md §4.4): the pool owns the connections and
    // outlives every lease it hands out.
    ConnectionPool* pool_{nullptr};
    Connection* connection_{nullptr};
};

// Fixed-size pool of driver connections.
//
// 🔴 Fixed size, full stop. No dynamic resizing, no autoscaling, no reconnect backoff, no health
// check thread, no circuit breaker and no read/write split. Every one of those is how a connection
// pool turns into a project of its own; the Phase-1 shape is a fixed pool plus a clear failure when
// it is empty.
//
// 🔴 Exactly ONE of those cuts came back, and only this far: a connection is checked when it is
// leased and reconnected at most once (Connection::EnsureAlive). What stays cut is everything in
// the list above — no backoff, no retry loop, no background health check. The retry is simply the
// next lease.
//
// It came back because the cut version was not "simple", it was broken: one MySQL restart, or
// wait_timeout expiring on an idle pool, killed every socket in here and then EVERY request failed
// forever while the process still reported healthy. A fixed pool that never recovers is not a
// smaller feature, it is a permanent outage waiting for a deploy.
//
// 🔴 architecture-design.md §9.1 — this is the ONE place in the layer where a lock is correct.
// The single-model rule is "concurrency is strands, and session state needs no lock because a
// strand serialises it". A connection pool is not session state: it is a genuinely shared resource
// that N unrelated DB threads contend for, which is exactly the exception §9.1 names alongside the
// global config cache. This mutex therefore does NOT contradict the lock-free session layer — it is
// on the other side of the line §9.1 draws. Without this comment the next reader reasonably reads
// it as a violation.
//
// 🔴 And it is a mutex rather than a counting semaphore even though §9.1 lists "pool size limiting"
// as a semaphore's legitimate use. A semaphore would give the count and nothing else, so the free
// list underneath would still need a mutex — two synchronisation objects where one does the job,
// and "take a permit, then pop the list" is not atomic across them. A semaphore earns its place
// when the thing being limited needs no shared structure; here it does.
class ConnectionPool {
public:
    // Opens all `size` connections eagerly. Throws DbException if any of them fails, so a bad
    // credential is a start-up failure rather than a surprise on the first request.
    ConnectionPool(const DbConnectionConfig& config, std::size_t size);
    ~ConnectionPool();

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;
    ConnectionPool(ConnectionPool&&) = delete;
    ConnectionPool& operator=(ConnectionPool&&) = delete;

    [[nodiscard]] std::size_t Size() const noexcept { return connections_.size(); }
    [[nodiscard]] std::size_t Available() const;

    // 🔴 NEVER waits forever. An exhausted pool with an unbounded wait turns one slow query into a
    // stalled server that looks like a hang rather than a fault, and the DB thread pool has a fixed
    // thread count so every waiter is a thread that stopped serving. std::nullopt on timeout is a
    // failure the caller can report; a hang is not.
    //
    // Expected failure, so nullopt rather than a throw (architecture-design.md §11.2a). Two
    // failures share that one return now: the pool had nothing free within the timeout, or the
    // connection it did have is dead and could not be re-established. Both are "no connection for
    // you right now", and the caller's answer to them is the same.
    [[nodiscard]] std::optional<PooledConnection> Acquire(Duration timeout);

private:
    friend class PooledConnection;
    void Release(Connection& connection) noexcept;

    std::vector<std::unique_ptr<Connection>> connections_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<Connection*> free_;
};

}  // namespace atlas
