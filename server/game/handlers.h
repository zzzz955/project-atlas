#pragma once
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/db/connection_pool.h"
#include "atlas/db/db_runner.h"
#include "atlas/net/acceptor.h"
#include "atlas/net/io_runner.h"
#include "atlas/net/net_types.h"
#include "atlas/net/session.h"
#include "atlas/proto/frame.h"
#include "atlas/redis/connection.h"
#include "atlas/redis/redis_runner.h"
#include "game/character_cache.h"
#include "game/equip_service.h"
#include "game/inventory.h"
#include "game/ranking.h"
#include "generated/db/characters_row.h"

// The GAME server: the opcode table, the handlers behind it, and the process wiring that serves
// them (architecture-design.md §5.1 — persistence · character · inventory, and the owner of the
// ORM).
//
// 🔴 THE OPCODE TABLE IS OWNED HERE, NOT BY THE CORE. The frame layer carries a `UInt16` and stops
// (§8.1, and cpp-style.md §8 says a proto file that learned what an opcode means would have to
// leave the framework asset list). The game registers `std::function`s; the core only ever accepts
// a callback. That is the same seam §14 draws between core and game, expressed as a call.
//
// 🔴 Why the server wiring lives beside the handlers rather than in main.cpp: main.cpp cannot be
// linked into a test executable, and the end-to-end proof for this slice is a test that connects a
// real socket and drives load → equip → reconnect. A `main()` that owned the wiring would force the
// test to build a second, different server — and then the thing under test would not be the thing
// that ships.

namespace atlas_demo {

// ── The wire contract this game speaks (§8.1 payload side) ───────────────────────────────────
// 🔴 Hand-written rather than generated, and the reason is stated instead of assumed: these
// messages have no contract source under shared/contracts, and adding one means re-running
// pkt_generator and committing its output. The payloads below still use the GENERATED codec
// helpers (generated/pkt/pkt_codec.h), so the little-endian rules of §8.5 come from one place.
inline constexpr atlas::UInt16 kOpCharacterLoadRequest = 0x0101;
inline constexpr atlas::UInt16 kOpCharacterLoadResponse = 0x0102;
inline constexpr atlas::UInt16 kOpEquipRequest = 0x0201;
inline constexpr atlas::UInt16 kOpEquipResponse = 0x0202;
inline constexpr atlas::UInt16 kOpRankingRequest = 0x0301;
inline constexpr atlas::UInt16 kOpRankingResponse = 0x0302;

// Result byte of kOpCharacterLoadResponse.
enum class LoadResult : atlas::UInt8 {
    Ok = 0,
    NotFound,
    // The DB thread pool ran the job without a connection: the pool was exhausted within its
    // acquire timeout. A clear failure rather than a hang (§10.3).
    Unavailable,
};

// Result byte of kOpEquipResponse. Extends EquipResult with the two refusals that are decided
// before the service is ever called.
inline constexpr atlas::UInt8 kEquipResponseNotLoaded = 0xFE;
inline constexpr atlas::UInt8 kEquipResponseUnavailable = 0xFF;

// Result byte of kOpRankingResponse.
enum class RankingResult : atlas::UInt8 {
    Ok = 0,
    // 🔴 NOT "the ranking is empty". The cache did not answer, and a client that drew an empty
    // leaderboard from this would be reporting an outage as a fact about the players. The load
    // path degrades silently because the database can answer in its place; this one cannot, and
    // saying so is the honest half of "the ranking is only a copy".
    Unavailable,
};

// ── Per-connection game state ────────────────────────────────────────────────────────────────

// 🔴 Every field is touched only from the session's strand: the frame handler runs there, and each
// DB completion is posted back there through the session's own executor. That is why there is not a
// lock in this struct (§9.1) — and why `send_seq` may be a plain counter even though §8.3 makes it
// per-connection state that two threads could otherwise reach.
struct SessionState {
    atlas::UInt16 server_id{0};
    atlas::CharacterId character_id{};
    bool loaded{false};
    // §8.3 — one send counter per direction, owned by the message layer rather than hidden inside
    // SendFrame (proto/session_framing.h says why).
    atlas::UInt32 send_seq{0};
};

// What a load produced, carried from the DB thread to the session strand.
struct CharacterSnapshot {
    LoadResult result{LoadResult::NotFound};
    atlas::generated::CharactersRow character{};
    std::vector<Item> items{};
};

// ── The cold load, exposed ───────────────────────────────────────────────────────────────────

// A seam that makes the commit fail on demand. 🔴 It is in the production signature for the same
// reason EquipService::FaultInjector is: the property this function defends — the ranking copy is
// written ONLY after the transaction committed — is not observable from outside unless the commit
// can be made not to happen. It is empty in production; the only caller that passes one is a test.
using LoadFaultInjector = std::function<void()>;

// Runs on a DB thread with a leased connection: reads the character and its items, stamps the
// login, commits, and only then records the exp into the ranking.
//
// 🔴 The ZADD sits after Commit() inside this function rather than in the caller's completion, and
// that placement is the whole guarantee. A completion decides from `result == Ok`, which is a
// convention someone can reorder; a statement after the commit in the same scope cannot be reached
// unless the commit returned, which is a fact of the control flow.
//
// `ranking` is a non-owning observer and may be null when no cache is configured (cpp-style.md
// §4.4).
void LoadCharacterOnDbThread(atlas::Ctx& ctx, atlas::Connection& connection,
                             atlas::UInt16 server_id, atlas::CharacterId character_id,
                             CharacterSnapshot& out, ExpRanking* ranking,
                             const LoadFaultInjector& before_commit = {});

// ── The dispatch table ───────────────────────────────────────────────────────────────────────

class HandlerTable {
public:
    using Handler = std::function<void(const std::shared_ptr<atlas::Session>&,
                                       const std::shared_ptr<SessionState>&, const atlas::Frame&)>;

    void Register(atlas::UInt16 opcode, Handler handler);

    // Non-owning observer; null means "this game does not know that opcode".
    [[nodiscard]] const Handler* Find(atlas::UInt16 opcode) const noexcept;

    [[nodiscard]] std::size_t Size() const noexcept { return handlers_.size(); }

private:
    std::unordered_map<atlas::UInt16, Handler> handlers_;
};

// ── The server ───────────────────────────────────────────────────────────────────────────────

class GameServer {
public:
    struct Options {
        atlas::Endpoint endpoint{};
        atlas::UInt16 server_id{1};
        // 0 = std::thread::hardware_concurrency (§9).
        std::size_t io_workers{0};
        std::size_t db_pool_size{4};
        std::size_t db_threads{2};
    };

    // Opens the listening socket and every database connection eagerly: a port that cannot be
    // taken or a credential that does not work is a start-up failure, not a surprise on the first
    // request.
    //
    // 🔴 THE CACHE IS THE EXACT OPPOSITE, AND THE DEFAULT ARGUMENT SAYS SO. An empty
    // `redis_config.host` means "no cache", and that server runs — every load simply goes to the
    // database, which is the same path a cache miss and a Redis outage take. A cache whose absence
    // could stop the server would have made the copy a dependency of the truth
    // (architecture-design.md §10.2).
    GameServer(const Options& options, const atlas::DbConnectionConfig& db_config,
               const atlas::RedisConnectionConfig& redis_config = {});
    ~GameServer();

    GameServer(const GameServer&) = delete;
    GameServer& operator=(const GameServer&) = delete;
    GameServer(GameServer&&) = delete;
    GameServer& operator=(GameServer&&) = delete;

    void Start();

    // 🔴 architecture-design.md §9 — THE ORDER IS THE CONTRACT, and it is idempotent:
    //
    //     acceptor.Stop()  ->  Close() every live session  ->  db_runner.Stop()  ->
    //     redis.Stop()  ->  io_runner.Stop()
    //
    // 🔴 The cache is stopped BEFORE the I/O pool and that is not cosmetic: its reconnect loop is
    // an outstanding operation on the io_context, so releasing the work guard while it is still
    // pending gives a queue that never drains and a Stop() that never returns.
    //
    // `io_runner.Stop()` only releases the work guard, so `run()` returns once the queue drains —
    // one socket with a pending read would otherwise hold the entire pool. Closing the front door
    // first is what makes the drain finite. The DB pool is drained between the sessions and the I/O
    // pool because a completion posts back onto a session strand: stopping the io_context first
    // would strand jobs that had already committed.
    void Stop() noexcept;

    [[nodiscard]] const atlas::Endpoint& LocalEndpoint() const noexcept;
    [[nodiscard]] std::size_t LiveSessionCount() const;
    [[nodiscard]] const HandlerTable& Handlers() const noexcept { return handlers_; }

private:
    void RegisterHandlers();
    void OnAccept(const std::shared_ptr<atlas::Session>& session);
    void Dispatch(const std::shared_ptr<atlas::Session>& session,
                  const std::shared_ptr<SessionState>& state, const atlas::Frame& frame);

    void HandleCharacterLoad(const std::shared_ptr<atlas::Session>& session,
                             const std::shared_ptr<SessionState>& state, const atlas::Frame& frame);
    void HandleEquip(const std::shared_ptr<atlas::Session>& session,
                     const std::shared_ptr<SessionState>& state, const atlas::Frame& frame);
    void HandleRanking(const std::shared_ptr<atlas::Session>& session,
                       const std::shared_ptr<SessionState>& state, const atlas::Frame& frame);

    // The second half of a load, entered once the cache has answered. `cached` empty means the
    // read missed (or Redis did not answer) and the database has to do the whole job.
    void SubmitCharacterLoad(const std::shared_ptr<atlas::Session>& session,
                             const std::shared_ptr<SessionState>& state, const atlas::Ctx& ctx,
                             atlas::CharacterId character_id,
                             std::optional<CachedCharacter> cached);

    void Forget(atlas::SessionId id);
    [[nodiscard]] atlas::Ctx MakeCtx(const std::shared_ptr<atlas::Session>& session,
                                     const std::shared_ptr<SessionState>& state);
    [[nodiscard]] static atlas::DbRunner::CompletionPoster PosterFor(
        const std::shared_ptr<atlas::Session>& session);

    Options options_;

    // 🔴 Declaration order is the destruction contract. The pool outlives the runner that leases
    // from it, and the io_context outlives the acceptor and every session bound to it, so those two
    // are declared first and destroyed last.
    atlas::ConnectionPool pool_;
    atlas::DbRunner db_runner_;
    atlas::IoRunner io_runner_;
    // 🔴 Declared AFTER io_runner_ so it is destroyed BEFORE it: this connection lives on that
    // io_context, and an object outliving the context it posts to is the one ordering mistake this
    // layer can make. There is no pool here for the reason atlas/redis/connection.h states —
    // boost-redis never blocks the thread it runs on, so there is nothing to isolate.
    atlas::RedisConnection redis_;
    atlas::RedisRunner redis_runner_;
    std::unique_ptr<atlas::SessionAcceptor> acceptor_;

    EquipService equip_service_;
    ExpRanking ranking_;
    CharacterCache character_cache_;
    HandlerTable handlers_;

    // Whether a cache was configured at all. Only Start() branches on it: everything below simply
    // asks the cache and handles "no" — which is what makes "Redis is down" and "there is no
    // Redis" one code path instead of two.
    bool redis_enabled_{false};

    // 🔴 §9.1 permits a lock on a genuinely shared resource, and this registry is one: the accept
    // strand inserts, N session strands erase, and the lifetime thread walks it during shutdown.
    // It is not session state — no strand serialises those three — so it is on the same side of the
    // line as the connection pool's mutex rather than in conflict with the lock-free session layer.
    // weak_ptr because the registry must not be what keeps a closed session alive.
    mutable std::mutex sessions_mutex_;
    std::unordered_map<atlas::UInt64, std::weak_ptr<atlas::Session>> sessions_;

    std::atomic<atlas::UInt64> next_trace_id_{1};
    bool running_{false};
};

}  // namespace atlas_demo
