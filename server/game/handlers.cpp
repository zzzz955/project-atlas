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
#include "atlas/redis/connection.h"
#include "game/character.h"
#include "game/character_cache.h"
#include "game/equip_service.h"
#include "game/inventory.h"
#include "game/ranking.h"
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

// How long start-up waits for the cache before carrying on without it. 🔴 Bounded and non-fatal:
// see the comment at the call site in Start().
constexpr atlas::Seconds kRedisReadyBudget{2};

// The WARM path's only write. Runs on a DB thread with a leased connection.
//
// 🔴 A single-row UPDATE is atomic without a transaction, so there is no scope here — and the one
// column it touches is the one a login owns. See kTouchCharacterLoginSql for why the cold path's
// whole-row UPDATE must not be reused from a cached copy.
void TouchLoginOnDbThread(atlas::Connection& connection, UInt16 server_id,
                          atlas::CharacterId character_id) {
    const std::array<DbValue, 3> parameters{DbValue{PersistableNow()},
                                            DbValue{static_cast<UInt64>(server_id)},
                                            DbValue{atlas::IdValue(character_id)}};
    connection.Prepare(kTouchCharacterLoginSql).Execute(parameters);
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

std::vector<Byte> EncodeRankingResponse(RankingResult result,
                                        const std::vector<RankEntry>& entries) {
    std::vector<Byte> payload;
    atlas::generated::WriteLe(payload, static_cast<UInt8>(result));
    if (result != RankingResult::Ok) {
        return payload;
    }
    const UInt16 count = atlas::generated::WriteLength(payload, entries.size());
    for (std::size_t index = 0; index < static_cast<std::size_t>(count); ++index) {
        atlas::generated::WriteLe(payload, atlas::IdValue(entries[index].character_id));
        atlas::generated::WriteLe(payload, entries[index].exp);
    }
    return payload;
}

}  // namespace

// ── The cold load ────────────────────────────────────────────────────────────────────────────

// 🔴 The login timestamp is written inside the same transaction that reads the character and its
// items, so a reader never sees "logged in at T" next to a character state from before T.
void LoadCharacterOnDbThread(atlas::Ctx& ctx, atlas::Connection& connection, UInt16 server_id,
                             atlas::CharacterId character_id, CharacterSnapshot& out,
                             ExpRanking* ranking, const LoadFaultInjector& before_commit) {
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

    if (before_commit) {
        before_commit();
    }
    transaction.Commit();

    // 🔴 EVERYTHING BELOW THIS LINE IS REACHABLE ONLY BECAUSE THE COMMIT RETURNED. That is the
    // ordering the ranking depends on (§10.2): the sorted set is a copy, and recording a number
    // the database might still refuse would leave the copy holding a value that never existed.
    if (ranking != nullptr) {
        ranking->Record(ctx, server_id, character.character_id_, character.exp_);
    }

    out.character = std::move(character);
    out.result = LoadResult::Ok;
}

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

GameServer::GameServer(const Options& options, const atlas::DbConnectionConfig& db_config,
                       const atlas::RedisConnectionConfig& redis_config)
    : options_(options),
      pool_(db_config, options.db_pool_size == 0 ? std::size_t{1} : options.db_pool_size),
      db_runner_(pool_, options.db_threads, atlas::Seconds{5}),
      io_runner_(options.io_workers),
      redis_(io_runner_.Context(), redis_config),
      redis_runner_(redis_),
      ranking_(redis_runner_),
      character_cache_(redis_runner_),
      redis_enabled_(!redis_config.host.empty()) {
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
    if (redis_enabled_) {
        // 🔴 After io_runner_.Start(), because the connection runs ON that context — and the wait
        // is bounded and NON-FATAL. A cache that refused to come up would have turned an optional
        // copy into a start-up dependency, which is exactly what §10.2 says it must not be.
        redis_.Start();
        if (!redis_.WaitUntilReady(kRedisReadyBudget)) {
            ATLAS_LOG_WARN(
                "redis is configured but did not answer within {}s - the character cache and the "
                "ranking degrade to the database until it does",
                kRedisReadyBudget.count());
        }
    }
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

    // 4. The cache, before the I/O pool. Its reconnect loop is an outstanding operation on that
    //    io_context, so releasing the work guard first leaves a queue that never drains.
    redis_.Stop();

    // 5. The I/O pool.
    io_runner_.Stop();
}

const atlas::Endpoint& GameServer::LocalEndpoint() const noexcept {
    return acceptor_->LocalEndpoint();
}

std::size_t GameServer::LiveSessionCount() const {
    const std::scoped_lock guard(sessions_mutex_);
    return sessions_.size();
}

GameServer::Counters GameServer::ReadCounters() const {
    // 🔴 Not a consistent snapshot, and it does not need to be: each field is read independently
    // while the server keeps running, so the queue depth may already have moved by the time the
    // rejection count is read. Freezing all five together would mean holding the queue mutex across
    // the read — buying a consistency nobody uses at the price of stalling the DB threads on the
    // interval the metrics are sampled.
    return Counters{.live_sessions = LiveSessionCount(),
                    .db_queue_depth = db_runner_.PendingCount(),
                    .db_queue_capacity = db_runner_.QueueCapacity(),
                    .db_rejected = db_runner_.RejectedCount(),
                    .db_acquire_failures = db_runner_.AcquireFailureCount(),
                    .db_acquire_wait_micros = db_runner_.AcquireWaitMicros()};
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
    handlers_.Register(kOpRankingRequest,
                       [this](const std::shared_ptr<atlas::Session>& session,
                              const std::shared_ptr<SessionState>& state,
                              const atlas::Frame& frame) { HandleRanking(session, state, frame); });
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
    const auto character_id = static_cast<atlas::CharacterId>(requested);
    ctx.character_id = character_id;

    // 🔴 The cache is asked FIRST and its answer only chooses how much work the database job does
    // — it never replaces the job. A login is a write, and no copy is allowed to absorb a write.
    // The completion below lands back on this session's strand, so `state` still needs no lock.
    character_cache_.Get(
        ctx, options_.server_id, character_id,
        [this, session, state, ctx, character_id](std::optional<CachedCharacter> cached) {
            SubmitCharacterLoad(session, state, ctx, character_id, std::move(cached));
        },
        PosterFor(session));
}

void GameServer::SubmitCharacterLoad(const std::shared_ptr<atlas::Session>& session,
                                     const std::shared_ptr<SessionState>& state,
                                     const atlas::Ctx& ctx, atlas::CharacterId character_id,
                                     std::optional<CachedCharacter> cached) {
    auto snapshot = std::make_shared<CharacterSnapshot>();
    auto warm = std::make_shared<std::optional<CachedCharacter>>(std::move(cached));
    const bool was_warm = warm->has_value();
    const UInt16 server_id = options_.server_id;

    const atlas::SubmitResult submitted = db_runner_.Submit(
        ctx,
        [this, snapshot, warm, server_id, character_id](atlas::Ctx& job_ctx,
                                                        atlas::Connection* connection) {
            if (connection == nullptr) {
                snapshot->result = LoadResult::Unavailable;
                return;
            }
            if (warm->has_value()) {
                // 🔴 A warm answer is a COPY, so a character deleted inside the TTL still reads
                // back Ok here. That is what a cache is; the TTL is the bound and the write path
                // invalidates every change this game can actually make.
                TouchLoginOnDbThread(*connection, server_id, character_id);
                snapshot->character = (*warm)->character;
                snapshot->items = (*warm)->items;
                snapshot->result = LoadResult::Ok;
                return;
            }
            LoadCharacterOnDbThread(job_ctx, *connection, server_id, character_id, *snapshot,
                                    &ranking_);
        },
        [this, session, state, snapshot, was_warm, server_id](const atlas::Ctx& done_ctx) {
            // On the session strand, so touching `state` and the send counter needs no lock.
            if (snapshot->result == LoadResult::Ok) {
                state->loaded = true;
                state->character_id = snapshot->character.character_id_;
                if (!was_warm) {
                    character_cache_.Put(done_ctx, server_id,
                                         CachedCharacter{.character = snapshot->character,
                                                         .items = snapshot->items});
                }
            }
            ++state->send_seq;
            atlas::SendFrame(*session, kOpCharacterLoadResponse, state->send_seq,
                             EncodeLoadResponse(*snapshot));
        },
        PosterFor(session));

    if (submitted != atlas::SubmitResult::Accepted) {
        // 🔴 REFUSED, NOT DROPPED. The job never ran and its completion never will, so the answer
        // has to be sent from here or the client waits for a response that does not exist — which
        // is what this path used to do (it logged and returned). LoadResult::Unavailable is the
        // code that already means "the database side could not serve you"; §10.3's exhausted pool
        // and a full queue are the same fact to a client, and the client's move is the same too.
        // Runs on the session strand — HandleCharacterLoad is dispatched there and the cache
        // completion is posted back to it — so `state` still needs no lock.
        snapshot->result = LoadResult::Unavailable;
        ++state->send_seq;
        atlas::SendFrame(*session, kOpCharacterLoadResponse, state->send_seq,
                         EncodeLoadResponse(*snapshot));
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
    const atlas::SubmitResult submitted = db_runner_.Submit(
        MakeCtx(session, state),
        [this, outcome, request](atlas::Ctx& job_ctx, atlas::Connection* connection) {
            if (connection == nullptr) {
                return;
            }
            *outcome = equip_service_.Equip(job_ctx, *connection, request);
        },
        [this, session, state, outcome, request](const atlas::Ctx& done_ctx) {
            const UInt8 code = outcome->has_value() ? static_cast<UInt8>((*outcome)->result)
                                                    : kEquipResponseUnavailable;
            const UInt64 unequipped = outcome->has_value() ? (*outcome)->unequipped_item_uid : 0;
            // 🔴 Invalidate, never update. The transaction has committed by the time this runs, so
            // the durable state is already the new one and the copy is simply wrong — dropping it
            // makes the next reader ask the database, while patching it would be a second writer
            // of the same fact (§10.2).
            if (outcome->has_value() && (*outcome)->result == EquipResult::Ok) {
                character_cache_.Invalidate(done_ctx, options_.server_id, request.character_id);
            }
            ++state->send_seq;
            atlas::SendFrame(*session, kOpEquipResponse, state->send_seq,
                             EncodeEquipResponse(code, request.item_uid,
                                                 static_cast<UInt8>(request.slot), unequipped));
        },
        PosterFor(session));

    if (submitted != atlas::SubmitResult::Accepted) {
        // 🔴 NO NEW RESULT CODE AND NO NEW OPCODE. kEquipResponseUnavailable is documented above as
        // one of "the two refusals decided before the service is ever called", and an overload
        // refusal is exactly that — decided before EquipService is reached. Inventing a code for it
        // would change the payload contract, and a contract change is a generator input plus the
        // Clarification Protocol, for a distinction the client acts on identically.
        //
        // 🔴 AND THE CONNECTION STAYS OPEN. Closing is the frame layer's answer to a PROTOCOL
        // VIOLATION — a lying length, a bad CRC, a sequence that went backwards (frame_reader.h).
        // Overload is not a violation; the peer did nothing wrong. Punishing it the same way erases
        // the distinction and turns a busy second into a reconnect storm.
        ++state->send_seq;
        atlas::SendFrame(*session, kOpEquipResponse, state->send_seq,
                         EncodeEquipResponse(kEquipResponseUnavailable, request.item_uid,
                                             static_cast<UInt8>(request.slot), 0));
    }
}

void GameServer::HandleRanking(const std::shared_ptr<atlas::Session>& session,
                               const std::shared_ptr<SessionState>& state,
                               const atlas::Frame& frame) {
    std::span<const Byte> cursor(frame.payload);
    UInt16 requested = 0;
    if (!atlas::generated::ReadLe(cursor, requested)) {
        ATLAS_LOG_WARN("session {} sent a truncated ranking request — closing",
                       atlas::IdValue(session->Id()));
        session->Close();
        return;
    }

    // 🔴 THE ONLY REQUEST THIS GAME SERVES ENTIRELY FROM THE CACHE, AND IT SAYS SO WHEN IT CANNOT.
    // The load path can fall back because the database holds the same facts; a ranking has no
    // database form here (it is a projection nobody stores), so "Redis is down" has to reach the
    // client as Unavailable rather than as an empty leaderboard.
    ranking_.Top(
        MakeCtx(session, state), options_.server_id, requested,
        [session, state](bool ok, const std::vector<RankEntry>& entries) {
            ++state->send_seq;
            atlas::SendFrame(*session, kOpRankingResponse, state->send_seq,
                             EncodeRankingResponse(
                                 ok ? RankingResult::Ok : RankingResult::Unavailable, entries));
        },
        PosterFor(session));
}

}  // namespace atlas_demo
