#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "atlas/core/types.h"
#include "atlas/net/net_types.h"
#include "atlas/proto/frame.h"
#include "atlas/proto/frame_reader.h"

// The interactive console client: `docker compose up`, then drive the demo by hand
// (architecture-design.md §15.3 for what the stack looks like, §16.1a for the rule this file obeys
// about not re-implementing the wire).
//
// 🔴 IT REUSES THE FRAME LAYER (atlas/proto), THE GENERATED LE CODEC AND THE DEMO GAME'S OPCODE
// TABLE (game/handlers.h) instead of encoding bytes of its own. The load harness states the reason
// (loadgen/load_client.h) and it holds here with one word changed: a second framing implementation
// carries its own bugs, and then a demo is a debugging session rather than a demo.
//
// 🔴 CONCURRENCY IS THE SERVER'S MODEL (§9.1): one io_context, one strand for the connection, and
// every asio handler entry point goes through atlas::Guarded (§11.2b). Reading stdin BLOCKS, so it
// never happens on an I/O thread — main() owns the terminal and posts each command onto the strand.
// That is the same separation §10.3 makes for the blocking database calls, for the same reason.
//
// 🔴 IT RENDERS NOTHING. There is no grid, no status widget, no history and no colour: this slice
// has neither a tick loop nor AoI (§3.2), so there is no moving server state to draw and a static
// grid would be a picture of something that does not exist.

namespace atlas_console {

// Runs on the connection strand once a round trip finished — or once the connection died, which is
// why it carries no result: the client has already printed what happened and the REPL only needs to
// know that it may prompt again. A completion that never fires would hang the terminal, so Close()
// fires the pending one as well.
using Completion = std::function<void()>;

// One connection, driven one request at a time.
//
// 🔴 Strictly one request in flight, because the REPL thread waits for `done` before it prompts
// again. That is what keeps the printed output from interleaving with what the user is typing, and
// it is also why none of the members below need a lock: they are touched only from this strand.
class ConsoleClient : public std::enable_shared_from_this<ConsoleClient> {
public:
    ConsoleClient(atlas::IoContext& io_context, atlas::Endpoint endpoint);

    ConsoleClient(const ConsoleClient&) = delete;
    ConsoleClient& operator=(const ConsoleClient&) = delete;
    ConsoleClient(ConsoleClient&&) = delete;
    ConsoleClient& operator=(ConsoleClient&&) = delete;
    ~ConsoleClient() = default;

    // Every entry point below may be called from ANY thread: each one posts onto the strand and
    // `done` runs there once the answer arrived (or the connection dropped).
    void Connect(Completion done);
    void RequestLoad(atlas::UInt64 character_id, Completion done);
    // The same opcode as RequestLoad — the load response is the only place the server publishes an
    // inventory — but it prints the item list alone. Issued after an equip it is a cold read, since
    // the equip invalidated the cached copy (§10.2), so it also shows that path working.
    void RequestInventory(Completion done);
    void RequestEquip(atlas::UInt64 item_uid, atlas::UInt8 slot, Completion done);
    void RequestRanking(atlas::UInt16 count, Completion done);

    void Shutdown();

    // Read from the REPL thread, hence atomic. False once the far side closed, a frame failed to
    // decode, or Shutdown() ran.
    [[nodiscard]] bool Alive() const noexcept { return !closed_.load(); }

private:
    void OnConnect(const atlas::ErrorCode& error);
    void ReadMore();
    void OnRead(const atlas::ErrorCode& error, std::size_t bytes_read);
    void OnFrame(const atlas::Frame& frame);

    void PrintLoadResponse(const atlas::Frame& frame);
    void PrintEquipResponse(const atlas::Frame& frame);
    void PrintRankingResponse(const atlas::Frame& frame);

    // Installs the completion for the round trip that is about to start. Any completion still
    // pending is fired first: dropping it would leave the REPL thread waiting forever.
    void BeginRequest(Completion done);
    void FinishRequest();

    void SendFrame(atlas::UInt16 opcode);
    void OnTransportError();
    void Close();

    atlas::Strand strand_;
    atlas::Socket socket_;
    atlas::Endpoint endpoint_;

    atlas::FrameReader reader_;
    std::vector<atlas::Frame> frames_;
    std::array<atlas::Byte, 4096> read_buffer_{};
    std::vector<atlas::Byte> payload_;
    std::vector<atlas::Byte> wire_;

    Completion pending_done_;
    // The response this client is waiting for. 0 means "nothing outstanding", which is not a valid
    // opcode in game/handlers.h and so cannot collide with one.
    atlas::UInt16 awaited_opcode_{0};
    bool items_only_{false};

    // The character this connection is bound to, set by a load the server answered with Ok. 🔴 The
    // server keeps its own copy and derives identity from the connection; this one exists only so
    // `inv` does not have to be re-told the id (§8.2 layer 3 — the client's copy is never what
    // authorises anything).
    atlas::UInt64 character_id_{0};
    bool loaded_{false};

    // §8.3 — one send counter per connection, owned by the layer that decides what to send rather
    // than hidden inside the core's SendFrame (proto/session_framing.h says why).
    atlas::UInt32 send_seq_{0};
    std::atomic<bool> closed_{false};
};

}  // namespace atlas_console
