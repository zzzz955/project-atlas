#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "atlas/config/secret_config.h"
#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/db/prepared_statement.h"
#include "atlas/net/net_types.h"
#include "atlas/proto/frame.h"
#include "atlas/proto/frame_reader.h"
#include "game/character.h"
#include "game/equip_service.h"
#include "game/handlers.h"
#include "game/inventory.h"
#include "game/npc.h"
#include "generated/db/character_items_row.h"
#include "generated/db/characters_row.h"
#include "generated/pkt/pkt_codec.h"

// The demo game's domain + the GAME server, end to end.
//
// 🔴 WHAT THIS SUITE CLAIMS.
//   * The "one item per equip slot" invariant survives a failure at the third write, because there
//     is no database constraint that could survive it for us (MySQL has no partial unique index —
//     server/db/schema.json says so, and that is why this file exists).
//   * Two concurrent equips on ONE character serialise, so the slot never ends up holding two.
//   * A request for an item the session's character does not own is refused rather than quietly
//     scoped away (§8.2 layer 3, server authority).
//   * A real client socket can drive load -> equip -> disconnect -> reconnect and see the equipment
//     still there.
//
// 🔴 WHAT IT DOES NOT CLAIM. There is no authentication: §12 (platform-auth JWT / JWKS) lands in a
// later slice, so the load request still names its own character. Everything AFTER the load derives
// identity from the connection, and that is the half of server authority this slice can claim. The
// end-to-end case below is a real socket speaking the real §8.1 frame format — it is not a client
// application, because `client/` is a later slice.
//
// 🔴 With no database reachable the DB half SKIPS, for the reason db_runtime_test.cpp gives: a
// green run that proved nothing is worse than a red one. The domain cases need no database and
// always run.

namespace {

using atlas::Byte;
using atlas::DbConnectionConfig;
using atlas::DbRow;
using atlas::DbValue;
using atlas::Int64;
using atlas::SysTime;
using atlas::UInt16;
using atlas::UInt32;
using atlas::UInt64;
using atlas::UInt8;
using atlas_demo::EquipResult;
using atlas_demo::EquipService;
using atlas_demo::EquipSlot;

// A server_id no other suite and no seeded data uses. db_runtime_test.cpp owns 30001 and
// tx_atomicity_test.cpp owns 30002.
constexpr UInt16 kServerId = 30003;

// 🔴 ONE ACCOUNT, TWO CHARACTERS — the ownership axis, seeded rather than asserted in a comment.
// An `Account -> Character` inheritance would have had no way to express this pair.
constexpr UInt64 kAccountUid = 9100;
constexpr UInt64 kCharacterA = 6100;
constexpr UInt64 kCharacterB = 6200;

// Character A holds two items; character B holds one, which A will be refused.
constexpr UInt64 kItemWorn = 7001;
constexpr UInt64 kItemSpare = 7002;
constexpr UInt64 kItemOfOther = 7900;
constexpr UInt64 kItemMissing = 7999;

constexpr std::string_view kDeleteItemsSql = "DELETE FROM `character_items` WHERE `server_id` = ?";
constexpr std::string_view kDeleteCharactersSql = "DELETE FROM `characters` WHERE `server_id` = ?";

std::optional<DbConnectionConfig> LoadGameConfig() {
    const atlas::SecretConfig secrets = atlas::SecretConfig::FromEnvironment();
    if (secrets.db_host.empty() || secrets.db_name.empty() || secrets.db_user.empty()) {
        return std::nullopt;
    }
    DbConnectionConfig config;
    config.host = secrets.db_host;
    config.port = secrets.db_port == 0 ? UInt16{3306} : secrets.db_port;
    config.database = secrets.db_name;
    config.user = secrets.db_user;
    config.password = secrets.db_password;
    // Same posture the GAME binary derives from the same variable — a suite that connected on
    // different TLS terms than the server would be testing a connection nobody ships.
    config.tls_no_verify = secrets.db_tls_no_verify;
#if defined(ATLAS_MARIADB_PLUGIN_DIR)
    config.plugin_directory = ATLAS_MARIADB_PLUGIN_DIR;
#endif
    return config;
}

// DATETIME in this schema has no fractional part.
SysTime NowToWholeSecond() {
    return std::chrono::floor<std::chrono::seconds>(atlas::SysClock::now());
}

template <class Predicate>
bool WaitUntil(Predicate predicate) {
    const atlas::TimePoint deadline = atlas::Clock::now() + atlas::Seconds{10};
    while (!predicate()) {
        if (atlas::Clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

// ── Reading the durable state back ───────────────────────────────────────────────────────────

std::optional<atlas::generated::CharacterItemsRow> ReadItem(atlas::Connection& connection,
                                                            UInt64 item_uid) {
    const std::array<DbValue, 2> parameters{DbValue{static_cast<UInt64>(kServerId)},
                                            DbValue{item_uid}};
    const std::vector<DbRow> rows =
        connection.Prepare(atlas_demo::kSelectItemByUidSql).Query(parameters);
    if (rows.empty()) {
        return std::nullopt;
    }
    return atlas_demo::CharacterItemsRowFromDb(rows.front());
}

UInt8 SlotOf(atlas::Connection& connection, UInt64 item_uid) {
    const std::optional<atlas::generated::CharacterItemsRow> row = ReadItem(connection, item_uid);
    return row.has_value() ? row->equip_slot_ : UInt8{0xFF};
}

std::size_t CountInSlot(atlas::Connection& connection, UInt64 character_id, EquipSlot slot) {
    const std::array<DbValue, 3> parameters{DbValue{static_cast<UInt64>(kServerId)},
                                            DbValue{character_id},
                                            DbValue{static_cast<UInt64>(static_cast<UInt8>(slot))}};
    return connection.Prepare(atlas_demo::kSelectItemsInSlotSql).Query(parameters).size();
}

std::optional<SysTime> LastLoginOf(atlas::Connection& connection, UInt64 character_id) {
    const std::array<DbValue, 2> parameters{DbValue{static_cast<UInt64>(kServerId)},
                                            DbValue{character_id}};
    const std::vector<DbRow> rows =
        connection.Prepare(atlas::generated::kCharactersSelectByPkSql).Query(parameters);
    if (rows.empty()) {
        return std::nullopt;
    }
    return atlas_demo::CharactersRowFromDb(rows.front()).last_login_at_;
}

// ── Seeding ──────────────────────────────────────────────────────────────────────────────────

void InsertCharacter(atlas::Connection& connection, UInt64 character_id, const std::string& name) {
    const std::array<DbValue, 10> parameters{DbValue{static_cast<UInt64>(kServerId)},
                                             DbValue{character_id},
                                             DbValue{kAccountUid},
                                             DbValue{name},
                                             DbValue{Int64{0}},
                                             DbValue{Int64{0}},
                                             DbValue{UInt64{1}},
                                             DbValue{UInt64{0}},
                                             DbValue{NowToWholeSecond()},
                                             DbValue{std::monostate{}}};
    connection.Prepare(atlas::generated::kCharactersInsertSql).Execute(parameters);
}

// 🔴 The owner is a strong-typed id and the item is a bare UInt64 on purpose: two adjacent
// parameters of the same width are exactly the swap cpp-style.md §4.3 exists to make impossible,
// and this call has both an owner and an item to get the wrong way round.
void InsertItem(atlas::Connection& connection, atlas::CharacterId character_id, UInt64 item_uid,
                UInt32 item_id, EquipSlot slot) {
    const std::array<DbValue, 6> parameters{DbValue{static_cast<UInt64>(kServerId)},
                                            DbValue{atlas::IdValue(character_id)},
                                            DbValue{item_uid},
                                            DbValue{static_cast<UInt64>(item_id)},
                                            DbValue{UInt64{1}},
                                            DbValue{static_cast<UInt64>(static_cast<UInt8>(slot))}};
    connection.Prepare(atlas::generated::kCharacterItemsInsertSql).Execute(parameters);
}

// ── The database fixture ─────────────────────────────────────────────────────────────────────

class GameEquipTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::optional<DbConnectionConfig> loaded = LoadGameConfig();
        if (!loaded.has_value()) {
            GTEST_SKIP() << "SKIPPED: no database configured. Set ATLAS_DB_HOST / ATLAS_DB_NAME / "
                            "ATLAS_DB_USER (and ATLAS_DB_PASSWORD) and start MySQL with "
                            "`docker compose --env-file server/.env up -d mysql`. Running from the "
                            "host, ATLAS_DB_HOST must be 127.0.0.1 rather than the compose service "
                            "name.";
        }
        config_ = *loaded;

        try {
            probe_ = std::make_unique<atlas::Connection>(config_);
        } catch (const std::exception& refused) {
            GTEST_SKIP() << "SKIPPED: ATLAS_DB_HOST is set but the database refused the "
                            "connection: "
                         << refused.what();
        }
        Cleanup(*probe_);
        Seed(*probe_);
    }

    void TearDown() override {
        if (probe_) {
            Cleanup(*probe_);
        }
    }

    static void Cleanup(atlas::Connection& connection) {
        const std::array<DbValue, 1> parameters{DbValue{static_cast<UInt64>(kServerId)}};
        connection.Prepare(kDeleteItemsSql).Execute(parameters);
        connection.Prepare(kDeleteCharactersSql).Execute(parameters);
    }

    // Character A wears kItemWorn in the weapon slot and carries kItemSpare loose, so every case
    // below starts from a state where the slot is genuinely occupied — an equip into an empty slot
    // would never exercise write 1.
    static void Seed(atlas::Connection& connection) {
        InsertCharacter(connection, kCharacterA, "atlas-a");
        InsertCharacter(connection, kCharacterB, "atlas-b");
        InsertItem(connection, static_cast<atlas::CharacterId>(kCharacterA), kItemWorn, 11,
                   EquipSlot::Weapon);
        InsertItem(connection, static_cast<atlas::CharacterId>(kCharacterA), kItemSpare, 12,
                   EquipSlot::None);
        InsertItem(connection, static_cast<atlas::CharacterId>(kCharacterB), kItemOfOther, 13,
                   EquipSlot::None);
    }

    static EquipService::Request RequestFor(UInt64 item_uid, EquipSlot slot,
                                            UInt64 character_id = kCharacterA) {
        return EquipService::Request{.server_id = kServerId,
                                     .character_id = static_cast<atlas::CharacterId>(character_id),
                                     .item_uid = item_uid,
                                     .slot = slot};
    }

    DbConnectionConfig config_{};
    std::unique_ptr<atlas::Connection> probe_;
};

// ── Case 1 — the happy path: three writes land and the slot stays unique ─────────────────────
TEST_F(GameEquipTest, EquippingWritesAllThreeAndLeavesExactlyOneItemInTheSlot) {
    EquipService service;
    atlas::Ctx ctx;
    ctx.trace_id = 6001;
    ctx.character_id = static_cast<atlas::CharacterId>(kCharacterA);

    ASSERT_FALSE(LastLoginOf(*probe_, kCharacterA).has_value());

    atlas::Connection worker(config_);
    const EquipService::Outcome outcome =
        service.Equip(ctx, worker, RequestFor(kItemSpare, EquipSlot::Weapon));

    EXPECT_EQ(outcome.result, EquipResult::Ok);
    EXPECT_EQ(outcome.unequipped_item_uid, kItemWorn);
    EXPECT_EQ(ctx.tx_state, atlas::TxState::Committed);

    // Read on a connection that never saw the transaction.
    atlas::Connection reader(config_);
    EXPECT_EQ(SlotOf(reader, kItemWorn), static_cast<UInt8>(EquipSlot::None));     // write 1
    EXPECT_EQ(SlotOf(reader, kItemSpare), static_cast<UInt8>(EquipSlot::Weapon));  // write 2
    EXPECT_TRUE(LastLoginOf(reader, kCharacterA).has_value());                     // write 3
    EXPECT_EQ(CountInSlot(reader, kCharacterA, EquipSlot::Weapon), std::size_t{1});
}

// ── Case 2 — a failure at the third write rolls everything back ──────────────────────────────
//
// 🔴 THE POINT OF THE WHOLE FILE. Writes 1 and 2 have already succeeded when the injected failure
// fires, so without a transaction the slot would be left EMPTY — the old item unequipped, the new
// one never equipped. That state breaks no database constraint (there is none to break), so nothing
// but the rollback stands between it and production.
TEST_F(GameEquipTest, AFailureAtTheThirdWriteRollsBackAndLeavesTheInvariantIntact) {
    EquipService service;
    atlas::Ctx ctx;
    ctx.trace_id = 6002;
    ctx.character_id = static_cast<atlas::CharacterId>(kCharacterA);

    atlas::Connection worker(config_);
    EXPECT_THROW(service.Equip(ctx, worker, RequestFor(kItemSpare, EquipSlot::Weapon),
                               [] { throw std::runtime_error("the third write failed"); }),
                 std::runtime_error);

    // ~Transaction rolled back on the way out, so the ledger says so rather than "Committed".
    EXPECT_EQ(ctx.tx_state, atlas::TxState::RolledBack);

    atlas::Connection reader(config_);
    EXPECT_EQ(SlotOf(reader, kItemWorn), static_cast<UInt8>(EquipSlot::Weapon));
    EXPECT_EQ(SlotOf(reader, kItemSpare), static_cast<UInt8>(EquipSlot::None));
    EXPECT_FALSE(LastLoginOf(reader, kCharacterA).has_value());
    EXPECT_EQ(CountInSlot(reader, kCharacterA, EquipSlot::Weapon), std::size_t{1});
}

// ── Case 3 — two equips on one character, at the same time ───────────────────────────────────
//
// 🔴 The interleaving that breaks the invariant is "both readers saw the slot occupied by the same
// item", and a plain SELECT under REPEATABLE READ takes no row lock — so InnoDB would not stop it.
// The per-character lock is what does. The first caller is HELD INSIDE its transaction until the
// second has entered Equip, which makes the overlap real instead of hoped for.
TEST_F(GameEquipTest, ConcurrentEquipsOnOneCharacterSerialiseAndTheSlotNeverHoldsTwo) {
    EquipService service;

    std::atomic<bool> first_inside{false};
    std::atomic<bool> second_entered{false};
    std::atomic<bool> failed{false};
    EquipResult first_result = EquipResult::InvalidSlot;
    EquipResult second_result = EquipResult::InvalidSlot;

    // Caught rather than allowed to escape: an exception leaving a std::thread terminates the whole
    // process and the run would report nothing useful.
    std::thread first([&] {
        try {
            atlas::Connection worker(config_);
            atlas::Ctx ctx;
            ctx.trace_id = 6003;
            first_result = service
                               .Equip(ctx, worker, RequestFor(kItemWorn, EquipSlot::Armor),
                                      [&] {
                                          first_inside.store(true);
                                          // Bounded, so a regression fails instead of hanging the
                                          // suite.
                                          WaitUntil([&] { return second_entered.load(); });
                                      })
                               .result;
        } catch (...) {
            failed.store(true);
        }
        // Whatever happened, release anyone still waiting on the gate.
        first_inside.store(true);
    });

    std::thread second([&] {
        try {
            WaitUntil([&] { return first_inside.load(); });
            second_entered.store(true);
            atlas::Connection worker(config_);
            atlas::Ctx ctx;
            ctx.trace_id = 6004;
            second_result =
                service.Equip(ctx, worker, RequestFor(kItemSpare, EquipSlot::Armor)).result;
        } catch (...) {
            failed.store(true);
        }
    });

    first.join();
    second.join();

    ASSERT_FALSE(failed.load());
    EXPECT_EQ(first_result, EquipResult::Ok);
    EXPECT_EQ(second_result, EquipResult::Ok);

    atlas::Connection reader(config_);
    // 🔴 The invariant, not the winner: the final state may be either item, but it may never be
    // both. A test that pinned the winner would be testing the scheduler.
    EXPECT_EQ(CountInSlot(reader, kCharacterA, EquipSlot::Armor), std::size_t{1});
    const UInt8 worn = SlotOf(reader, kItemWorn);
    const UInt8 spare = SlotOf(reader, kItemSpare);
    EXPECT_NE(worn == static_cast<UInt8>(EquipSlot::Armor),
              spare == static_cast<UInt8>(EquipSlot::Armor));
}

// ── Case 4 — an item that does not exist ─────────────────────────────────────────────────────
TEST_F(GameEquipTest, EquippingAnItemThatDoesNotExistFailsAndWritesNothing) {
    EquipService service;
    atlas::Ctx ctx;
    ctx.trace_id = 6005;

    atlas::Connection worker(config_);
    const EquipService::Outcome outcome =
        service.Equip(ctx, worker, RequestFor(kItemMissing, EquipSlot::Weapon));

    EXPECT_EQ(outcome.result, EquipResult::ItemNotFound);
    // 🔴 No transaction was ever opened — the refusal happens before write 1, so there is nothing
    // to roll back and the ledger stays at None.
    EXPECT_EQ(ctx.tx_state, atlas::TxState::None);

    atlas::Connection reader(config_);
    EXPECT_EQ(SlotOf(reader, kItemWorn), static_cast<UInt8>(EquipSlot::Weapon));
    EXPECT_FALSE(LastLoginOf(reader, kCharacterA).has_value());
}

// ── Case 5 — another character's item ────────────────────────────────────────────────────────
//
// 🔴 §8.2 layer 3. The item is REAL and the request is well formed; what makes it illegal is who is
// asking. Nothing in the frame layer can catch this — a checksum and an HMAC would both pass — so
// the server has to be the one that says no.
TEST_F(GameEquipTest, EquippingAnotherCharactersItemIsRefused) {
    EquipService service;
    atlas::Ctx ctx;
    ctx.trace_id = 6006;

    atlas::Connection worker(config_);
    const EquipService::Outcome outcome =
        service.Equip(ctx, worker, RequestFor(kItemOfOther, EquipSlot::Weapon));

    EXPECT_EQ(outcome.result, EquipResult::NotOwned);
    EXPECT_EQ(ctx.tx_state, atlas::TxState::None);

    atlas::Connection reader(config_);
    EXPECT_EQ(SlotOf(reader, kItemOfOther), static_cast<UInt8>(EquipSlot::None));
    EXPECT_EQ(SlotOf(reader, kItemWorn), static_cast<UInt8>(EquipSlot::Weapon));
    EXPECT_EQ(CountInSlot(reader, kCharacterB, EquipSlot::Weapon), std::size_t{0});
}

// ── End to end, over a real socket ───────────────────────────────────────────────────────────
//
// 🔴 THIS IS THE "CLIENT CONNECTS" PROOF, AND WHAT IT IS IS STATED PLAINLY: there is no client
// application (`client/` is a later slice), so the client here is this test — a real TCP socket
// speaking the real §8.1 frame format against the real acceptor / session / frame / DB stack. It is
// not a shortcut around the socket; that is the one thing it must not be, because the socket is
// what is being proven.

// A synchronous client driven by the test thread, so the assertions read top to bottom.
class TestClient {
public:
    explicit TestClient(const atlas::Endpoint& endpoint) : socket_(io_context_) {
        socket_.connect(endpoint);
    }

    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;

    ~TestClient() {
        atlas::ErrorCode ignored;
        socket_.shutdown(atlas::Socket::shutdown_both, ignored);
        socket_.close(ignored);
    }

    // §8.3 — the client keeps its own send counter, exactly as the server keeps one for its
    // direction. A repeat or a regression closes the connection (frame_reader.h).
    void Send(UInt16 opcode, const std::vector<Byte>& payload) {
        std::vector<Byte> wire;
        ++send_seq_;
        ASSERT_TRUE(atlas::EncodeFrame(wire, opcode, send_seq_, payload));
        atlas::asio::write(socket_, atlas::asio::buffer(wire));
    }

    // Reads the fixed header, then exactly the payload it declares.
    atlas::Frame Receive() {
        std::vector<Byte> header(atlas::kFrameHeaderSize);
        atlas::asio::read(socket_, atlas::asio::buffer(header));
        atlas::FrameHeader decoded;
        EXPECT_TRUE(atlas::DecodeFrameHeader(header, decoded));

        std::vector<Byte> payload(decoded.length);
        if (decoded.length != 0) {
            atlas::asio::read(socket_, atlas::asio::buffer(payload));
        }
        // Verified rather than trusted: the checksum is the framing-integrity layer (§8.2 layer 1)
        // and a client that skipped it would not be exercising it.
        EXPECT_EQ(atlas::FrameChecksum(decoded.opcode, decoded.seq, payload), decoded.crc32);
        return atlas::Frame{.opcode = decoded.opcode, .seq = decoded.seq, .payload = payload};
    }

private:
    atlas::IoContext io_context_;
    atlas::Socket socket_;
    UInt32 send_seq_{0};
};

struct LoadedCharacter {
    UInt8 result{0xFF};
    UInt64 character_id{0};
    UInt64 account_uid{0};
    std::string name{};
    UInt16 level{0};
    std::vector<atlas_demo::Item> items{};
};

LoadedCharacter ParseLoadResponse(const atlas::Frame& frame) {
    LoadedCharacter parsed;
    std::span<const Byte> cursor(frame.payload);
    EXPECT_TRUE(atlas::generated::ReadLe(cursor, parsed.result));
    if (parsed.result != static_cast<UInt8>(atlas_demo::LoadResult::Ok)) {
        return parsed;
    }

    atlas::Int32 pos_x = 0;
    atlas::Int32 pos_y = 0;
    UInt64 exp = 0;
    UInt16 count = 0;
    EXPECT_TRUE(atlas::generated::ReadLe(cursor, parsed.character_id));
    EXPECT_TRUE(atlas::generated::ReadLe(cursor, parsed.account_uid));
    EXPECT_TRUE(atlas::generated::ReadUtf8(cursor, parsed.name));
    EXPECT_TRUE(atlas::generated::ReadLe(cursor, pos_x));
    EXPECT_TRUE(atlas::generated::ReadLe(cursor, pos_y));
    EXPECT_TRUE(atlas::generated::ReadLe(cursor, parsed.level));
    EXPECT_TRUE(atlas::generated::ReadLe(cursor, exp));
    EXPECT_TRUE(atlas::generated::ReadLe(cursor, count));
    for (UInt16 index = 0; index < count; ++index) {
        atlas_demo::Item item;
        UInt8 slot = 0;
        EXPECT_TRUE(atlas::generated::ReadLe(cursor, item.item_uid));
        EXPECT_TRUE(atlas::generated::ReadLe(cursor, item.item_id));
        EXPECT_TRUE(atlas::generated::ReadLe(cursor, item.stack_count));
        EXPECT_TRUE(atlas::generated::ReadLe(cursor, slot));
        item.slot = static_cast<EquipSlot>(slot);
        parsed.items.push_back(item);
    }
    return parsed;
}

std::vector<Byte> LoadRequestPayload(UInt64 character_id) {
    std::vector<Byte> payload;
    atlas::generated::WriteLe(payload, character_id);
    return payload;
}

std::vector<Byte> EquipRequestPayload(UInt64 item_uid, EquipSlot slot) {
    std::vector<Byte> payload;
    atlas::generated::WriteLe(payload, item_uid);
    atlas::generated::WriteLe(payload, static_cast<UInt8>(slot));
    return payload;
}

const atlas_demo::Item* FindItem(const LoadedCharacter& loaded, UInt64 item_uid) {
    for (const atlas_demo::Item& item : loaded.items) {
        if (item.item_uid == item_uid) {
            return &item;
        }
    }
    return nullptr;
}

TEST_F(GameEquipTest, AClientLoadsEquipsReconnectsAndTheEquipmentIsStillThere) {
    atlas_demo::GameServer::Options options;
    // Loopback rather than the any-address, and port 0 so parallel ctest runs cannot collide.
    options.endpoint = atlas::Endpoint(atlas::asio::ip::address_v4::loopback(), 0);
    options.server_id = kServerId;
    options.io_workers = 2;
    options.db_pool_size = 2;
    options.db_threads = 2;

    atlas_demo::GameServer server(options, config_);
    server.Start();

    {
        TestClient client(server.LocalEndpoint());

        client.Send(atlas_demo::kOpCharacterLoadRequest, LoadRequestPayload(kCharacterA));
        const atlas::Frame load_response = client.Receive();
        ASSERT_EQ(load_response.opcode, atlas_demo::kOpCharacterLoadResponse);

        const LoadedCharacter loaded = ParseLoadResponse(load_response);
        ASSERT_EQ(loaded.result, static_cast<UInt8>(atlas_demo::LoadResult::Ok));
        EXPECT_EQ(loaded.character_id, kCharacterA);
        EXPECT_EQ(loaded.account_uid, kAccountUid);
        EXPECT_EQ(loaded.name, "atlas-a");
        ASSERT_EQ(loaded.items.size(), std::size_t{2});
        ASSERT_NE(FindItem(loaded, kItemSpare), nullptr);
        EXPECT_EQ(FindItem(loaded, kItemSpare)->slot, EquipSlot::None);

        client.Send(atlas_demo::kOpEquipRequest,
                    EquipRequestPayload(kItemSpare, EquipSlot::Trinket));
        const atlas::Frame equip_response = client.Receive();
        ASSERT_EQ(equip_response.opcode, atlas_demo::kOpEquipResponse);

        std::span<const Byte> cursor(equip_response.payload);
        UInt8 code = 0xFF;
        UInt64 echoed_uid = 0;
        UInt8 echoed_slot = 0;
        UInt64 unequipped = 0;
        ASSERT_TRUE(atlas::generated::ReadLe(cursor, code));
        ASSERT_TRUE(atlas::generated::ReadLe(cursor, echoed_uid));
        ASSERT_TRUE(atlas::generated::ReadLe(cursor, echoed_slot));
        ASSERT_TRUE(atlas::generated::ReadLe(cursor, unequipped));
        EXPECT_EQ(code, static_cast<UInt8>(EquipResult::Ok));
        EXPECT_EQ(echoed_uid, kItemSpare);
        EXPECT_EQ(echoed_slot, static_cast<UInt8>(EquipSlot::Trinket));
        // The trinket slot was empty, so nothing came off.
        EXPECT_EQ(unequipped, UInt64{0});

        // 🔴 A refusal the frame layer cannot see: character A asking for character B's item over a
        // perfectly valid frame. The server is the only thing that can say no (§8.2 layer 3).
        client.Send(atlas_demo::kOpEquipRequest,
                    EquipRequestPayload(kItemOfOther, EquipSlot::Weapon));
        const atlas::Frame refused = client.Receive();
        std::span<const Byte> refusal(refused.payload);
        UInt8 refusal_code = 0;
        ASSERT_TRUE(atlas::generated::ReadLe(refusal, refusal_code));
        EXPECT_EQ(refusal_code, static_cast<UInt8>(EquipResult::NotOwned));
    }  // ← the client disconnects here

    ASSERT_TRUE(WaitUntil([&] { return server.LiveSessionCount() == 0; }));

    // Reconnect. A brand new connection, a brand new session, and the equipment is read back out of
    // the database rather than out of anything the first connection left in memory.
    {
        TestClient reconnected(server.LocalEndpoint());
        reconnected.Send(atlas_demo::kOpCharacterLoadRequest, LoadRequestPayload(kCharacterA));
        const LoadedCharacter loaded = ParseLoadResponse(reconnected.Receive());

        ASSERT_EQ(loaded.result, static_cast<UInt8>(atlas_demo::LoadResult::Ok));
        ASSERT_NE(FindItem(loaded, kItemSpare), nullptr);
        EXPECT_EQ(FindItem(loaded, kItemSpare)->slot, EquipSlot::Trinket);
        // The weapon slot is untouched: the refused request wrote nothing.
        ASSERT_NE(FindItem(loaded, kItemWorn), nullptr);
        EXPECT_EQ(FindItem(loaded, kItemWorn)->slot, EquipSlot::Weapon);
    }

    // architecture-design.md §9 — front door, then sessions, then the pools. Stop() is idempotent
    // and the destructor calls it too; calling it here is what puts the order under test.
    server.Stop();
    EXPECT_EQ(server.LiveSessionCount(), std::size_t{0});
}

// ── The domain model — no database, so these always run ──────────────────────────────────────

// 🔴 THE DISTINCTION THIS FILE EXISTS TO PIN. If the next reader "simplifies" Account and Character
// into an inheritance chain, this test stops compiling — which is the only kind of documentation
// that survives.
TEST(DomainOwnershipTest, AnAccountOwnsManyCharactersAndIsNotABaseClassOfThem) {
    static_assert(!std::is_base_of_v<atlas_demo::Account, atlas_demo::Character>,
                  "Account -> Character is OWNERSHIP (1:N), never inheritance: an is-a chain "
                  "cannot express a second character of the same account");

    atlas_demo::Account account(static_cast<atlas::AccountId>(kAccountUid));
    const atlas_demo::Character first(kServerId, static_cast<atlas::CharacterId>(kCharacterA),
                                      static_cast<atlas::AccountId>(kAccountUid), "atlas-a", 0, 0,
                                      1, 0);
    const atlas_demo::Character second(kServerId, static_cast<atlas::CharacterId>(kCharacterB),
                                       static_cast<atlas::AccountId>(kAccountUid), "atlas-b", 0, 0,
                                       1, 0);

    account.Own(first.CharacterKey());
    account.Own(second.CharacterKey());
    // Idempotent: owning the same character twice is not two characters.
    account.Own(second.CharacterKey());

    EXPECT_EQ(account.Characters().size(), std::size_t{2});
    EXPECT_TRUE(account.Owns(first.CharacterKey()));
    EXPECT_TRUE(account.Owns(second.CharacterKey()));
    EXPECT_FALSE(account.Owns(static_cast<atlas::CharacterId>(kItemMissing)));
    EXPECT_EQ(first.AccountUid(), second.AccountUid());
}

// The other axis: Entity → Character / Npc really is is-a, and the dispatch is real because there
// are two derived types rather than one.
TEST(DomainInheritanceTest, EntityDispatchesToCharacterAndNpc) {
    const atlas_demo::Character character(kServerId, static_cast<atlas::CharacterId>(kCharacterA),
                                          static_cast<atlas::AccountId>(kAccountUid), "atlas-a", 3,
                                          4, 7, 120);
    const atlas_demo::Npc npc(static_cast<atlas::ActorId>(42), 900, -3, -4, 2, "training dummy");

    const std::array<const atlas_demo::Entity*, 2> world{&character, &npc};
    EXPECT_EQ(world[0]->Kind(), atlas_demo::EntityKind::Character);
    EXPECT_EQ(world[1]->Kind(), atlas_demo::EntityKind::Npc);
    EXPECT_EQ(world[0]->DisplayName(), "atlas-a");
    EXPECT_EQ(world[1]->DisplayName(), "training dummy");
    EXPECT_EQ(world[0]->Level(), UInt16{7});
    EXPECT_EQ(world[1]->PosX(), atlas::Int32{-3});
    // The world identity is derived from the durable one — see the Character constructor.
    EXPECT_EQ(atlas::IdValue(world[0]->Id()), kCharacterA);
}

TEST(DomainInventoryTest, EquippingClearsTheSlotItMovesInto) {
    atlas_demo::Inventory inventory;
    inventory.Add(atlas_demo::Item{
        .item_uid = kItemWorn, .item_id = 11, .stack_count = 1, .slot = EquipSlot::Weapon});
    inventory.Add(atlas_demo::Item{
        .item_uid = kItemSpare, .item_id = 12, .stack_count = 1, .slot = EquipSlot::None});

    ASSERT_TRUE(inventory.Equip(kItemSpare, EquipSlot::Weapon));

    ASSERT_NE(inventory.EquippedAt(EquipSlot::Weapon), nullptr);
    EXPECT_EQ(inventory.EquippedAt(EquipSlot::Weapon)->item_uid, kItemSpare);
    ASSERT_NE(inventory.Find(kItemWorn), nullptr);
    EXPECT_FALSE(inventory.Find(kItemWorn)->Equipped());
}

TEST(DomainInventoryTest, SlotZeroIsNotAnEquipTargetAndAnUnheldItemIsRefused) {
    atlas_demo::Inventory inventory;
    inventory.Add(atlas_demo::Item{
        .item_uid = kItemWorn, .item_id = 11, .stack_count = 1, .slot = EquipSlot::None});

    // 🔴 "Equip into slot 0" is an unequip wearing an equip's clothes; letting it through would
    // make the two indistinguishable in the log.
    EXPECT_FALSE(inventory.Equip(kItemWorn, EquipSlot::None));
    EXPECT_FALSE(inventory.Equip(kItemMissing, EquipSlot::Weapon));
    EXPECT_FALSE(IsEquippableSlot(static_cast<EquipSlot>(atlas_demo::kEquipSlotCount + 1)));
    EXPECT_TRUE(IsEquippableSlot(EquipSlot::Trinket));
}

}  // namespace
