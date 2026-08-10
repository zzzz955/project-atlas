#pragma once
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
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
#include "game/equip_service.h"
#include "game/inventory.h"
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
    GameServer(const Options& options, const atlas::DbConnectionConfig& db_config);
    ~GameServer();

    GameServer(const GameServer&) = delete;
    GameServer& operator=(const GameServer&) = delete;
    GameServer(GameServer&&) = delete;
    GameServer& operator=(GameServer&&) = delete;

    void Start();

    // 🔴 architecture-design.md §9 — THE ORDER IS THE CONTRACT, and it is idempotent:
    //
    //     acceptor.Stop()  ->  Close() every live session  ->  db_runner.Stop()  ->
    //     io_runner.Stop()
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
    std::unique_ptr<atlas::SessionAcceptor> acceptor_;

    EquipService equip_service_;
    HandlerTable handlers_;

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
