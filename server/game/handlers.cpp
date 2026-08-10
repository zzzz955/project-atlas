#include "game/handlers.h"

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/error.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/db/prepared_statement.h"
#include "atlas/db/transaction.h"
#include "atlas/net/net_types.h"
#include "atlas/net/session.h"
#include "atlas/proto/frame.h"
#include "atlas/proto/session_framing.h"
#include "game/character.h"
#include "game/equip_service.h"
#include "game/inventory.h"
#include "generated/db/character_items_row.h"
#include "generated/db/characters_row.h"
#include "generated/pkt/pkt_codec.h"

namespace atlas_demo {

namespace {

using atlas::Byte;
using atlas::DbRow;
using atlas::DbValue;
using atlas::UInt16;
using atlas::UInt32;
using atlas::UInt64;
using atlas::UInt8;

// Runs on a DB thread with a leased connection.
//
// 🔴 The login timestamp is written inside the same transaction that reads the character and its
// items, so a reader never sees "logged in at T" next to a character state from before T.
void LoadCharacterOnDbThread(atlas::Ctx& ctx, atlas::Connection& connection, UInt16 server_id,
                             atlas::CharacterId character_id, CharacterSnapshot& out) {
    const std::array<DbValue, 2> key{DbValue{static_cast<UInt64>(server_id)},
                                     DbValue{atlas::IdValue(character_id)}};

    atlas::Transaction transaction(connection, ctx);

    const std::vector<DbRow> character_rows =
        connection.Prepare(atlas::generated::kCharactersSelectByPkSql).Query(key);
    if (character_rows.empty()) {
        // Nothing was written, so letting the scope unwind is the rollback (§10).
        out.result = LoadResult::NotFound;
        return;
    }

    atlas::generated::CharactersRow character = CharactersRowFromDb(character_rows.front());
    character.last_login_at_ = PersistableNow();
    connection.Prepare(atlas::generated::kCharactersUpdateByPkSql)
        .Execute(CharactersUpdateParameters(character));

    const std::vector<DbRow> item_rows = connection.Prepare(kSelectItemsByCharacterSql).Query(key);
    out.items.clear();
    out.items.reserve(item_rows.size());
    for (const DbRow& row : item_rows) {
        out.items.push_back(ItemFromRow(CharacterItemsRowFromDb(row)));
    }

    transaction.Commit();

    out.character = std::move(character);
    out.result = LoadResult::Ok;
}

// ── Payload codecs ───────────────────────────────────────────────────────────────────────────
// 🔴 The little-endian primitives come from the GENERATED codec (generated/pkt/pkt_codec.h) rather
// than being written out again here, so §8.5's wire rules have one implementation. What is
// hand-written is only the field order of these two messages, because no contract source declares
// them yet.

std::vector<Byte> EncodeLoadResponse(const CharacterSnapshot& snapshot) {
    std::vector<Byte> payload;
    atlas::generated::WriteLe(payload, static_cast<UInt8>(snapshot.result));
    if (snapshot.result != LoadResult::Ok) {
        return payload;
    }

    atlas::generated::WriteLe(payload, atlas::IdValue(snapshot.character.character_id_));
    atlas::generated::WriteLe(payload, atlas::IdValue(snapshot.character.account_uid_));
    atlas::generated::WriteUtf8(payload, snapshot.character.name_);
    atlas::generated::WriteLe(payload, snapshot.character.pos_x_);
    atlas::generated::WriteLe(payload, snapshot.character.pos_y_);
    atlas::generated::WriteLe(payload, snapshot.character.level_);
    atlas::generated::WriteLe(payload, snapshot.character.exp_);

    // A repeated field's prefix counts ELEMENTS (§8.5). The clamp is the generated helper's, so an
    // oversized list cannot desynchronise the reader.
    const UInt16 count = atlas::generated::WriteLength(payload, snapshot.items.size());
    for (std::size_t index = 0; index < static_cast<std::size_t>(count); ++index) {
        const Item& item = snapshot.items[index];
        atlas::generated::WriteLe(payload, item.item_uid);
        atlas::generated::WriteLe(payload, item.item_id);
        atlas::generated::WriteLe(payload, item.stack_count);
        atlas::generated::WriteLe(payload, static_cast<UInt8>(item.slot));
    }
    return payload;
}

std::vector<Byte> EncodeEquipResponse(UInt8 result, UInt64 item_uid, UInt8 slot,
                                      UInt64 unequipped_item_uid) {
    std::vector<Byte> payload;
    atlas::generated::WriteLe(payload, result);
    atlas::generated::WriteLe(payload, item_uid);
    atlas::generated::WriteLe(payload, slot);
    atlas::generated::WriteLe(payload, unequipped_item_uid);
    return payload;
}

}  // namespace

// ── HandlerTable ─────────────────────────────────────────────────────────────────────────────

void HandlerTable::Register(atlas::UInt16 opcode, Handler handler) {
    ATLAS_CHECK(static_cast<bool>(handler), "opcode 0x{:04x} was registered with an empty handler",
                opcode);
    const auto inserted = handlers_.emplace(opcode, std::move(handler));
    // 🔴 A second registration for one opcode is a programming error, not a policy choice:
    // whichever one wins is decided by insertion order, and that is exactly the kind of thing that
    // changes when a file is renamed.
    ATLAS_CHECK(inserted.second, "opcode 0x{:04x} is registered twice", opcode);
}

const HandlerTable::Handler* HandlerTable::Find(atlas::UInt16 opcode) const noexcept {
    const auto found = handlers_.find(opcode);
    return found == handlers_.end() ? nullptr : &found->second;
}

// ── GameServer ───────────────────────────────────────────────────────────────────────────────

GameServer::GameServer(const Options& options, const atlas::DbConnectionConfig& db_config)
    : options_(options),
      pool_(db_config, options.db_pool_size == 0 ? std::size_t{1} : options.db_pool_size),
      db_runner_(pool_, options.db_threads, atlas::Seconds{5}),
      io_runner_(options.io_workers) {
    acceptor_ = std::make_unique<atlas::SessionAcceptor>(
        io_runner_.Context(), options.endpoint,
        [this](const std::shared_ptr<atlas::Session>& session) { OnAccept(session); });
    RegisterHandlers();
}

GameServer::~GameServer() { Stop(); }

void GameServer::Start() {
    if (running_) {
        return;
    }
    running_ = true;
    acceptor_->Start();
    db_runner_.Start();
    io_runner_.Start();
    ATLAS_LOG_INFO("GAME listening on {}:{} (server_id={}, io_workers={}, db_threads={})",
                   acceptor_->LocalEndpoint().address().to_string(),
                   acceptor_->LocalEndpoint().port(), options_.server_id, io_runner_.WorkerCount(),
                   db_runner_.ThreadCount());
}

void GameServer::Stop() noexcept {
    if (!running_) {
        return;
    }
    running_ = false;

    // 1. The front door.
    acceptor_->Stop();

    // 2. Every live session. Collected under the lock and closed outside it: Close() posts onto the
    //    session's strand, and a close handler that ran inline would re-enter Forget().
    std::vector<std::shared_ptr<atlas::Session>> live;
    {
        const std::scoped_lock guard(sessions_mutex_);
        live.reserve(sessions_.size());
        for (const auto& entry : sessions_) {
            if (std::shared_ptr<atlas::Session> session = entry.second.lock()) {
                live.push_back(std::move(session));
            }
        }
        sessions_.clear();
    }
    for (const std::shared_ptr<atlas::Session>& session : live) {
        session->Close();
    }

    // 3. The DB pool, before the I/O pool. A completion posts back onto a session strand, so
    //    releasing the io_context first would strand jobs that had already committed.
    db_runner_.Stop();

    // 4. The I/O pool.
    io_runner_.Stop();
}

const atlas::Endpoint& GameServer::LocalEndpoint() const noexcept {
    return acceptor_->LocalEndpoint();
}

std::size_t GameServer::LiveSessionCount() const {
    const std::scoped_lock guard(sessions_mutex_);
    return sessions_.size();
}

void GameServer::RegisterHandlers() {
    handlers_.Register(
        kOpCharacterLoadRequest,
        [this](const std::shared_ptr<atlas::Session>& session,
               const std::shared_ptr<SessionState>& state,
               const atlas::Frame& frame) { HandleCharacterLoad(session, state, frame); });
    handlers_.Register(kOpEquipRequest,
                       [this](const std::shared_ptr<atlas::Session>& session,
                              const std::shared_ptr<SessionState>& state,
                              const atlas::Frame& frame) { HandleEquip(session, state, frame); });
}

void GameServer::OnAccept(const std::shared_ptr<atlas::Session>& session) {
    // Runs on the accept strand, after Session::Create and before Session::Start() — the one window
    // in which the session's callbacks may still be installed.
    auto state = std::make_shared<SessionState>();
    state->server_id = options_.server_id;

    atlas::AttachFrameReader(session, [this, state](const std::shared_ptr<atlas::Session>& source,
                                                    const atlas::Frame& frame) {
        // 🔴 Already inside the session's Guarded read handler (net/session.cpp), so a throw here
        // is caught, logged and closes this connection instead of killing the I/O thread (§11.2b).
        Dispatch(source, state, frame);
    });
    session->SetCloseHandler(
        [this](const std::shared_ptr<atlas::Session>& closed) { Forget(closed->Id()); });

    const std::scoped_lock guard(sessions_mutex_);
    sessions_.emplace(atlas::IdValue(session->Id()), session);
}

void GameServer::Forget(atlas::SessionId id) {
    const std::scoped_lock guard(sessions_mutex_);
    sessions_.erase(atlas::IdValue(id));
}

void GameServer::Dispatch(const std::shared_ptr<atlas::Session>& session,
                          const std::shared_ptr<SessionState>& state, const atlas::Frame& frame) {
    const HandlerTable::Handler* handler = handlers_.Find(frame.opcode);
    if (handler == nullptr) {
        // 🔴 Closed, not ignored. An opcode this game does not know is not a message it partly
        // understood — it is a peer speaking a different protocol, and continuing to read from it
        // means guessing. Same verdict a framing error gets (§8.1).
        ATLAS_LOG_WARN("session {} sent unknown opcode 0x{:04x} — closing",
                       atlas::IdValue(session->Id()), frame.opcode);
        session->Close();
        return;
    }
    (*handler)(session, state, frame);
}

atlas::Ctx GameServer::MakeCtx(const std::shared_ptr<atlas::Session>& session,
                               const std::shared_ptr<SessionState>& state) {
    atlas::Ctx ctx;
    ctx.trace_id = next_trace_id_.fetch_add(1);
    ctx.session_id = session->Id();
    ctx.character_id = state->character_id;
    return ctx;
}

atlas::DbRunner::CompletionPoster GameServer::PosterFor(
    const std::shared_ptr<atlas::Session>& session) {
    // A copy of the strand, not a reference to the session: the poster outlives this frame and the
    // strand is a cheap value type.
    atlas::Strand strand = session->GetStrand();
    return [strand](std::function<void()> completion) {
        atlas::asio::post(strand, std::move(completion));
    };
}

void GameServer::HandleCharacterLoad(const std::shared_ptr<atlas::Session>& session,
                                     const std::shared_ptr<SessionState>& state,
                                     const atlas::Frame& frame) {
    std::span<const Byte> cursor(frame.payload);
    UInt64 requested = 0;
    if (!atlas::generated::ReadLe(cursor, requested)) {
        ATLAS_LOG_WARN("session {} sent a truncated character load — closing",
                       atlas::IdValue(session->Id()));
        session->Close();
        return;
    }

    // 🔴 THE REQUEST NAMES ITS OWN CHARACTER, AND THAT IS A HOLE — a named one. Authentication is
    // §12 (platform-auth JWT / JWKS) and it lands in a later slice, so nothing yet proves this
    // connection may play this character. What DOES hold today is everything after the load: the
    // session's bound character is set only once the database confirmed the row, and every later
    // request derives its identity from the connection instead of from the packet. That is the half
    // of server authority (§8.2 layer 3) this slice can actually claim.
    atlas::Ctx ctx = MakeCtx(session, state);
    ctx.character_id = static_cast<atlas::CharacterId>(requested);

    auto snapshot = std::make_shared<CharacterSnapshot>();
    const UInt16 server_id = options_.server_id;
    const bool queued = db_runner_.Submit(
        ctx,
        [snapshot, server_id, requested](atlas::Ctx& job_ctx, atlas::Connection* connection) {
            if (connection == nullptr) {
                snapshot->result = LoadResult::Unavailable;
                return;
            }
            LoadCharacterOnDbThread(job_ctx, *connection, server_id,
                                    static_cast<atlas::CharacterId>(requested), *snapshot);
        },
        [session, state, snapshot](const atlas::Ctx&) {
            // On the session strand, so touching `state` and the send counter needs no lock.
            if (snapshot->result == LoadResult::Ok) {
                state->loaded = true;
                state->character_id = snapshot->character.character_id_;
            }
            ++state->send_seq;
            atlas::SendFrame(*session, kOpCharacterLoadResponse, state->send_seq,
                             EncodeLoadResponse(*snapshot));
        },
        PosterFor(session));

    if (!queued) {
        ATLAS_LOG_WARN("character load dropped: the DB runner is not accepting work");
    }
}

void GameServer::HandleEquip(const std::shared_ptr<atlas::Session>& session,
                             const std::shared_ptr<SessionState>& state,
                             const atlas::Frame& frame) {
    if (!state->loaded) {
        // 🔴 Refused here rather than trusting a character id from the packet. Without a loaded
        // session there is no server-side identity to act on, and inventing one from the request is
        // the exact shape of the bug §8.2 layer 3 exists to stop.
        ++state->send_seq;
        atlas::SendFrame(*session, kOpEquipResponse, state->send_seq,
                         EncodeEquipResponse(kEquipResponseNotLoaded, 0, 0, 0));
        return;
    }

    std::span<const Byte> cursor(frame.payload);
    UInt64 item_uid = 0;
    UInt8 raw_slot = 0;
    if (!atlas::generated::ReadLe(cursor, item_uid) ||
        !atlas::generated::ReadLe(cursor, raw_slot)) {
        ATLAS_LOG_WARN("session {} sent a truncated equip request — closing",
                       atlas::IdValue(session->Id()));
        session->Close();
        return;
    }

    const EquipService::Request request{.server_id = options_.server_id,
                                        .character_id = state->character_id,
                                        .item_uid = item_uid,
                                        .slot = static_cast<EquipSlot>(raw_slot)};

    // Empty means the job never produced one — no connection was leased, or the work threw and the
    // DB thread's guard swallowed it (§11.2b). Either way the client is told, rather than left
    // waiting for a response that is not coming.
    auto outcome = std::make_shared<std::optional<EquipService::Outcome>>();
    const bool queued = db_runner_.Submit(
        MakeCtx(session, state),
        [this, outcome, request](atlas::Ctx& job_ctx, atlas::Connection* connection) {
            if (connection == nullptr) {
                return;
            }
            *outcome = equip_service_.Equip(job_ctx, *connection, request);
        },
        [session, state, outcome, request](const atlas::Ctx&) {
            const UInt8 code = outcome->has_value() ? static_cast<UInt8>((*outcome)->result)
                                                    : kEquipResponseUnavailable;
            const UInt64 unequipped = outcome->has_value() ? (*outcome)->unequipped_item_uid : 0;
            ++state->send_seq;
            atlas::SendFrame(*session, kOpEquipResponse, state->send_seq,
                             EncodeEquipResponse(code, request.item_uid,
                                                 static_cast<UInt8>(request.slot), unequipped));
        },
        PosterFor(session));

    if (!queued) {
        ATLAS_LOG_WARN("equip request dropped: the DB runner is not accepting work");
    }
}

}  // namespace atlas_demo
