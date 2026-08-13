#include "atlas/db/connection_pool.h"

#include <memory>
#include <utility>

#include "atlas/core/error.h"
#include "atlas/core/log.h"

namespace atlas {

PooledConnection::PooledConnection(ConnectionPool& pool, Connection& connection) noexcept
    : pool_(&pool), connection_(&connection) {}

PooledConnection::PooledConnection(PooledConnection&& other) noexcept
    : pool_(other.pool_), connection_(other.connection_) {
    other.pool_ = nullptr;
    other.connection_ = nullptr;
}

PooledConnection& PooledConnection::operator=(PooledConnection&& other) noexcept {
    if (this != &other) {
        Return();
        pool_ = other.pool_;
        connection_ = other.connection_;
        other.pool_ = nullptr;
        other.connection_ = nullptr;
    }
    return *this;
}

PooledConnection::~PooledConnection() { Return(); }

void PooledConnection::Return() noexcept {
    if (pool_ != nullptr && connection_ != nullptr) {
        pool_->Release(*connection_);
    }
    pool_ = nullptr;
    connection_ = nullptr;
}

ConnectionPool::ConnectionPool(const DbConnectionConfig& config, std::size_t size) {
    ATLAS_CHECK(size > 0, "connection pool size must be greater than zero");
    connections_.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
        connections_.push_back(std::make_unique<Connection>(config));
        free_.push_back(connections_.back().get());
    }
    ATLAS_LOG_INFO("db connection pool ready: {} connections", size);
}

ConnectionPool::~ConnectionPool() {
    // 🔴 A lease outliving the pool is a dangling pointer inside PooledConnection::Return, and the
    // destructor cannot throw its way out of that. It can at least refuse to be silent.
    const std::lock_guard<std::mutex> lock(mutex_);
    if (free_.size() != connections_.size()) {
        // Same shape as Guarded::Fail (core/error.h): a destructor is implicitly noexcept and
        // std::format runs at this call site, so the diagnostic itself must not be able to throw.
        try {
            ATLAS_LOG_ERROR("db connection pool destroyed with {} connection(s) still leased",
                            connections_.size() - free_.size());
        } catch (...) {  // NOLINT — nowhere left to report to.
        }
    }
}

std::size_t ConnectionPool::Available() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return free_.size();
}

std::optional<PooledConnection> ConnectionPool::Acquire(Duration timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    // wait_for on the steady Clock (core/time.h). 🔴 Not the wall clock: an NTP correction that
    // moves system_clock backwards would turn a bounded wait into the unbounded one this whole
    // function exists to prevent (cpp-style.md §4.2).
    if (!available_.wait_for(lock, timeout, [this] { return !free_.empty(); })) {
        ATLAS_LOG_WARN("db connection pool exhausted: {} connections, none free within timeout",
                       connections_.size());
        return std::nullopt;
    }

    Connection* connection = free_.front();
    free_.pop_front();
    lock.unlock();

    // 🔴 Validated OUTSIDE the lock, and that is the whole reason the unlock is here. EnsureAlive
    // does network I/O (a ping, and on a bad day a full reconnect); doing it while holding mutex_
    // would block every other thread's Acquire AND Release behind one socket, which is exactly the
    // "a mutex is for a genuinely shared resource, briefly" justification above stopping being
    // true. The connection is already off the free list, so no one else can touch it meanwhile.
    //
    // The lease is built BEFORE the check so that both exits give the connection back through the
    // same RAII path — a dead connection returns to the pool and gets its next chance on the next
    // lease, which is what makes the recovery spread over time instead of looping here.
    PooledConnection lease(*this, *connection);
    if (!connection->EnsureAlive()) {
        ATLAS_LOG_WARN("db connection dead on lease and reconnect failed; reporting unavailable");
        return std::nullopt;
    }
    return {std::move(lease)};
}

void ConnectionPool::Release(Connection& connection) noexcept {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        free_.push_back(&connection);
    }
    available_.notify_one();
}

}  // namespace atlas
