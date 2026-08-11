#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/net/net_types.h"

// architecture-design.md 10.2 - the cache client. This header is the umbrella of the layer: the
// settings, the command/reply vocabulary, and the connection itself.
//
// 🔴 THIS LAYER DOES NOT KNOW THE GAME. core-purity (15.4) forbids server/atlas/** from including
// anything under the generated tree and from carrying game vocabulary, so nothing here names a
// key, a value shape or a lifetime policy. A command arrives already stringified and a reply
// leaves flattened; every key format lives in the game layer above. That is the same seam
// atlas/db draws with DbValue (10.3), expressed for a different store.
//
// 🔴 THERE IS NO PUB/SUB API HERE, AND THERE MUST NEVER BE ONE. 10.2 calls that the single most
// important rule of the whole section, and it is a consequence of the 5.2 mandate: two WORLD
// servers talking over a Redis channel keep "no direct WORLD to WORLD connection" to the letter
// while destroying its meaning, and the seat Phase 3's Relay was going to take disappears with
// it. A wrapper around the fan-out commands is not neutral plumbing - it is the affordance, and
// the next person who needs cross-server messaging will use whatever is already there. 🔴 The
// regression line is mechanical and it is a grep for those two command names over this directory
// returning nothing, so not even a comment here spells them.
//
// 🔴 THERE IS NO SEPARATE THREAD POOL HERE, AND THE ASYMMETRY WITH atlas/db IS DELIBERATE.
// 10.3 gives the database a pool of its own because libmariadb's C API is synchronous: a query
// blocks the thread that issues it, so running one on an io_context worker freezes every session
// that worker serves. boost-redis is asio-native - it never blocks a thread, it yields it - so
// the reason that pool exists simply does not apply. What this layer needs instead is
// serialisation, and 9.1 already has the single answer for that: a strand.
//
// 🔴 NO RETRY, NO BACKOFF, NO CIRCUIT BREAKER. A failure is returned as a failure and the caller
// decides. This is the same restraint 10.3 states as its "deliberately out of scope" list, and
// for the cache it is more than a preference: the read cache is a copy, the database is the
// source of truth, and the correct answer to "Redis did not respond" is to go and ask the
// database, not to wait here.
//
// Out of scope (stated, not implied): cluster, sentinel, pipelining tuning, Lua scripting.

namespace atlas {

// 🔴 Every field comes from `.env` via atlas::SecretConfig (5.4), never from server.ini, and no
// field is ever logged or formatted into an exception message. The struct exists so that this
// layer does not have to link the config layer to be handed its credentials.
struct RedisConnectionConfig {
    std::string host{};
    UInt16 port{6379};
    std::string password{};
};

// One command: the verb plus its already-stringified arguments.
//
// 🔴 Strings, not a variant. Redis speaks RESP bulk strings and nothing else, so a value type
// here would only be a second spelling of std::to_string at the wrong layer. The caller that
// knows what a score or an id means is the caller that should format it.
struct RedisCommand {
    std::string verb{};
    std::vector<std::string> args{};
};

// One reply, flattened depth-first. std::nullopt is a RESP null; aggregate headers contribute
// nothing of their own, so `ZREVRANGE ... WITHSCORES` arrives as member, score, member, score.
using RedisReply = std::vector<std::optional<std::string>>;

struct RedisResult {
    // False means the command did not complete: no connection, a cancelled operation, or a
    // server-side error. 🔴 It is never "the key was missing" - that is a successful reply
    // carrying a null, and the two must not be confused by the caller.
    bool ok{false};
    RedisReply values{};
};

// One multiplexed connection to a Redis server, living on an existing io_context.
//
// 🔴 EVERY OPERATION IS DISPATCHED ONTO ONE STRAND. boost::redis::connection is not thread safe,
// and 9.1 makes strands the single concurrency model rather than one of two - so the answer here
// is the same one the session layer uses, and there is no mutex in this class.
class RedisConnection {
public:
    // Invoked on this connection's strand once the command completed or failed.
    using Handler = std::function<void(const RedisResult&)>;

    RedisConnection(IoContext& io_context, const RedisConnectionConfig& config);
    ~RedisConnection();

    RedisConnection(const RedisConnection&) = delete;
    RedisConnection& operator=(const RedisConnection&) = delete;
    RedisConnection(RedisConnection&&) = delete;
    RedisConnection& operator=(RedisConnection&&) = delete;

    // Starts the connect/reconnect loop on the io_context. Non-blocking, and it does NOT fail
    // when the server is unreachable: 🔴 a cache that refuses to start would turn "Redis is
    // down" into "the game server is down", which is the exact inversion of what a copy of the
    // truth is for.
    void Start();

    // 🔴 Must run before IoRunner::Stop(). The run loop is an outstanding operation on the
    // io_context, so a context whose work guard is released while this is still pending never
    // drains. Idempotent.
    void Stop() noexcept;

    // Blocks until a PING round-trips or the deadline passes. For the process lifetime thread
    // (start-up) and for tests - 🔴 never call it from an I/O thread, it would wait on itself.
    [[nodiscard]] bool WaitUntilReady(Duration timeout);

    // Whether a host was supplied at construction. 🔴 THIS IS NOT "IS THE SERVER REACHABLE": a
    // configured server that is currently down still answers true, and that is the whole point of
    // the question. `RedisResult::ok == false` cannot carry the difference - it says the command
    // did not complete, not whether there was ever a cache for it to complete against - so a
    // caller that wants to report a failed command has to ask this first. 10.2 makes the cache
    // optional and its absence a warning rather than a failure; without this the two collapse and
    // "the deployment turned the cache off" gets logged as "the cache is broken", once per request.
    [[nodiscard]] bool IsConfigured() const noexcept;

    // Queues one command. `handler` always runs exactly once, so a caller never waits forever for
    // an answer that is not coming — a stopped connection is refused inline and a server that does
    // not respond is bounded by the per-command deadline in the .cpp.
    void Execute(Ctx ctx, const RedisCommand& command, Handler handler);

private:
    // pimpl: 🔴 the mechanical half of "no boost/redis header is visible to a consumer of this
    // library", the other half being the PRIVATE link in CMakeLists.txt. Same shape atlas/db
    // uses to keep <mysql.h> out of every translation unit.
    //
    // 🔴 shared_ptr, not unique_ptr, and the reason is teardown: Stop() posts the cancellation
    // onto the strand, so an I/O thread may still be holding it after this object is gone. The
    // handler keeps its own reference and the last one out destroys the driver.
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace atlas
