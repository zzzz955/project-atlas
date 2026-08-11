#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
#include "atlas/net/io_runner.h"
#include "atlas/net/net_types.h"
#include "atlas/proto/frame.h"
#include "atlas/redis/connection.h"
#include "atlas/redis/redis_runner.h"
#include "game/character.h"
#include "game/character_cache.h"
#include "game/handlers.h"
#include "game/inventory.h"
#include "game/ranking.h"
#include "generated/db/characters_row.h"
#include "generated/pkt/pkt_codec.h"

// The cache axis (architecture-design.md §10.2): the exp ranking, the character read cache, and the
// one property that decides whether either of them is allowed to exist.
//
// 🔴 WHAT THIS SUITE CLAIMS.
//   * The ranking is ordered by the authoritative `characters.exp`, highest first.
//   * The read cache misses, then hits, and a WRITE drops the copy rather than patching it.
//   * 🔴 WITH REDIS UNREACHABLE A CHARACTER STILL LOADS, over a real socket, from the database.
//     This is the case the whole design rests on: the cache is a copy and the database is the
//     source of truth, so an outage of the copy is not an outage of the server.
//   * A transaction that does not commit leaves the ranking untouched — and the committing run
//     right after it proves that assertion is not vacuous.
//
// 🔴 WHAT IT DOES NOT CLAIM. Nothing about pub/sub, because there is no such API to test: §10.2
// forbids it as the bypass around the §5.2 WORLD ↔ WORLD mandate, and `atlas/redis` does not wrap
// SUBSCRIBE or PUBLISH at all. Nothing about eviction under memory pressure, cluster or sentinel
// either — all three are out of scope by the same section.
//
// 🔴 With no database or no Redis reachable the suite SKIPS loudly, for the reason
// db_runtime_test.cpp gives: a green run that proved nothing is worse than a red one.

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
using atlas_demo::CachedCharacter;
using atlas_demo::EquipSlot;
using atlas_demo::RankEntry;

// A server_id no other suite uses. db_runtime_test.cpp owns 30001, tx_atomicity_test.cpp 30002 and
// game_equip_test.cpp 30003.
constexpr UInt16 kServerId = 30004;

constexpr UInt64 kAccountUid = 9400;
constexpr UInt64 kCharacterLow = 6401;
constexpr UInt64 kCharacterMid = 6402;
constexpr UInt64 kCharacterHigh = 6403;

// 🔴 Deliberately not in insertion order, so an assertion on the ranking cannot pass by accident
// on a store that returned the members exactly as they went in.
constexpr UInt64 kExpLow = 100;
constexpr UInt64 kExpMid = 5000;
constexpr UInt64 kExpHigh = 90000;

constexpr UInt64 kItemHeld = 7401;

constexpr std::string_view kDeleteItemsSql = "DELETE FROM `character_items` WHERE `server_id` = ?";
constexpr std::string_view kDeleteCharactersSql = "DELETE FROM `characters` WHERE `server_id` = ?";

// 🔴 A port nothing listens on. This is how "Redis is down" is injected: stopping the container
// would make the test depend on the developer's docker state and would take the OTHER suites down
// with it, and a mock would prove that the mock returns errors rather than that boost-redis does.
constexpr UInt16 kDeadRedisPort = 1;

std::optional<DbConnectionConfig> LoadDbConfig() {
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
    config.tls_no_verify = secrets.db_tls_no_verify;
#if defined(ATLAS_MARIADB_PLUGIN_DIR)
    config.plugin_directory = ATLAS_MARIADB_PLUGIN_DIR;
#endif
    return config;
}

atlas::RedisConnectionConfig LoadRedisConfig() {
    const atlas::SecretConfig secrets = atlas::SecretConfig::FromEnvironment();
    atlas::RedisConnectionConfig config;
    config.host = secrets.redis_host;
    config.port = secrets.redis_port == 0 ? UInt16{6379} : secrets.redis_port;
    config.password = secrets.redis_password;
    // 🔴 The gate rewrites ATLAS_DB_HOST from the compose service name to the loopback because it
    // always runs on the host; nothing does that for Redis, so this suite does it itself. An unset
    // host takes the same branch: compose publishes 6379 on the host, and skipping because a
    // variable is missing would be skipping a service that is actually running.
    if (config.host.empty() || config.host == "redis") {
        config.host = "127.0.0.1";
    }
    return config;
}

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

void InsertCharacter(atlas::Connection& connection, UInt64 character_id, const std::string& name,
                     UInt64 exp) {
    const std::array<DbValue, 10> parameters{DbValue{static_cast<UInt64>(kServerId)},
                                             DbValue{character_id},
                                             DbValue{kAccountUid},
                                             DbValue{name},
                                             DbValue{Int64{0}},
                                             DbValue{Int64{0}},
                                             DbValue{UInt64{1}},
                                             DbValue{exp},
                                             DbValue{NowToWholeSecond()},
                                             DbValue{std::monostate{}}};
    connection.Prepare(atlas::generated::kCharactersInsertSql).Execute(parameters);
}

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

UInt8 SlotOf(atlas::Connection& connection, UInt64 item_uid) {
    const std::array<DbValue, 2> parameters{DbValue{static_cast<UInt64>(kServerId)},
                                            DbValue{item_uid}};
    const std::vector<DbRow> rows =
        connection.Prepare(atlas_demo::kSelectItemByUidSql).Query(parameters);
    if (rows.empty()) {
        return 0xFF;
    }
    return atlas_demo::CharacterItemsRowFromDb(rows.front()).equip_slot_;
}

// ── A real socket, speaking the real §8.1 frame format ───────────────────────────────────────
// The same shape game_equip_test.cpp uses, and duplicated rather than shared for the reason that
// file gives: two suites in two executables must not share a file-local helper, because with unity
// build ON one target is one translation unit and the anonymous namespaces would collide.

class TestClient {
public:
    explicit TestClient(const atlas::Endpoint& endpoint) : socket_(io_context_) {
        socket_.connect(endpoint);
    }

    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;
    TestClient(TestClient&&) = delete;
    TestClient& operator=(TestClient&&) = delete;

    ~TestClient() {
        atlas::ErrorCode ignored;
        socket_.shutdown(atlas::Socket::shutdown_both, ignored);
        socket_.close(ignored);
    }

    void Send(UInt16 opcode, const std::vector<Byte>& payload) {
        std::vector<Byte> wire;
        ++send_seq_;
        ASSERT_TRUE(atlas::EncodeFrame(wire, opcode, send_seq_, payload));
        atlas::asio::write(socket_, atlas::asio::buffer(wire));
    }

    atlas::Frame Receive() {
        std::vector<Byte> header(atlas::kFrameHeaderSize);
        atlas::asio::read(socket_, atlas::asio::buffer(header));
        atlas::FrameHeader decoded;
        EXPECT_TRUE(atlas::DecodeFrameHeader(header, decoded));

        std::vector<Byte> payload(decoded.length);
        if (decoded.length != 0) {
            atlas::asio::read(socket_, atlas::asio::buffer(payload));
        }
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
    UInt64 exp{0};
    std::string name{};
    std::vector<atlas_demo::Item> items{};
};

LoadedCharacter ParseLoadResponse(const atlas::Frame& frame) {
    LoadedCharacter parsed;
    std::span<const Byte> cursor(frame.payload);
    EXPECT_TRUE(atlas::generated::ReadLe(cursor, parsed.result));
    if (parsed.result != static_cast<UInt8>(atlas_demo::LoadResult::Ok)) {
        return parsed;
    }

    UInt64 account_uid = 0;
    atlas::Int32 pos_x = 0;
    atlas::Int32 pos_y = 0;
    UInt16 level = 0;
    UInt16 count = 0;
    EXPECT_TRUE(atlas::generated::ReadLe(cursor, parsed.character_id));
    EXPECT_TRUE(atlas::generated::ReadLe(cursor, account_uid));
    EXPECT_TRUE(atlas::generated::ReadUtf8(cursor, parsed.name));
    EXPECT_TRUE(atlas::generated::ReadLe(cursor, pos_x));
    EXPECT_TRUE(atlas::generated::ReadLe(cursor, pos_y));
    EXPECT_TRUE(atlas::generated::ReadLe(cursor, level));
    EXPECT_TRUE(atlas::generated::ReadLe(cursor, parsed.exp));
    EXPECT_TRUE(atlas::generated::ReadLe(cursor, count));
    for (UInt16 index = 0; index < count; ++index) {
        atlas_demo::Item entry;
        UInt8 slot = 0;
        EXPECT_TRUE(atlas::generated::ReadLe(cursor, entry.item_uid));
        EXPECT_TRUE(atlas::generated::ReadLe(cursor, entry.item_id));
        EXPECT_TRUE(atlas::generated::ReadLe(cursor, entry.stack_count));
        EXPECT_TRUE(atlas::generated::ReadLe(cursor, slot));
        entry.slot = static_cast<EquipSlot>(slot);
        parsed.items.push_back(entry);
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
    for (const atlas_demo::Item& entry : loaded.items) {
        if (entry.item_uid == item_uid) {
            return &entry;
        }
    }
    return nullptr;
}

atlas_demo::GameServer::Options TestServerOptions() {
    atlas_demo::GameServer::Options options;
    // Loopback rather than the any-address, and port 0 so parallel ctest runs cannot collide.
    options.endpoint = atlas::Endpoint(atlas::asio::ip::address_v4::loopback(), 0);
    options.server_id = kServerId;
    options.io_workers = 2;
    options.db_pool_size = 2;
    options.db_threads = 2;
    return options;
}

// ── The fixture ──────────────────────────────────────────────────────────────────────────────

class GameCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::optional<DbConnectionConfig> loaded = LoadDbConfig();
        if (!loaded.has_value()) {
            GTEST_SKIP() << "SKIPPED: no database configured. Set ATLAS_DB_HOST / ATLAS_DB_NAME / "
                            "ATLAS_DB_USER (and ATLAS_DB_PASSWORD) and start MySQL with "
                            "`docker compose --env-file server/.env up -d mysql`.";
        }
        db_config_ = *loaded;

        try {
            probe_ = std::make_unique<atlas::Connection>(db_config_);
        } catch (const std::exception& refused) {
            GTEST_SKIP() << "SKIPPED: ATLAS_DB_HOST is set but the database refused the "
                            "connection: "
                         << refused.what();
        }
        Cleanup(*probe_);
        Seed(*probe_);

        redis_config_ = LoadRedisConfig();
        io_runner_ = std::make_unique<atlas::IoRunner>(1);
        io_runner_->Start();
        redis_ = std::make_unique<atlas::RedisConnection>(io_runner_->Context(), redis_config_);
        redis_->Start();
        if (!redis_->WaitUntilReady(atlas::Seconds{5})) {
            GTEST_SKIP() << "SKIPPED: no Redis reachable at " << redis_config_.host << ":"
                         << redis_config_.port
                         << ". Start it with `docker compose --env-file server/.env up -d redis`.";
        }

        runner_ = std::make_unique<atlas::RedisRunner>(*redis_);
        ranking_ = std::make_unique<atlas_demo::ExpRanking>(*runner_);
        cache_ = std::make_unique<atlas_demo::CharacterCache>(*runner_);
        ClearRedisKeys();
    }

    void TearDown() override {
        if (probe_) {
            Cleanup(*probe_);
        }
        if (runner_) {
            ClearRedisKeys();
        }
        cache_.reset();
        ranking_.reset();
        runner_.reset();
        if (redis_) {
            // 🔴 The connection before the io_context, always: its run loop is an outstanding
            // operation on that context and a released work guard would never drain past it.
            redis_->Stop();
            redis_.reset();
        }
        if (io_runner_) {
            io_runner_->Stop();
            io_runner_.reset();
        }
    }

    static void Cleanup(atlas::Connection& connection) {
        const std::array<DbValue, 1> parameters{DbValue{static_cast<UInt64>(kServerId)}};
        connection.Prepare(kDeleteItemsSql).Execute(parameters);
        connection.Prepare(kDeleteCharactersSql).Execute(parameters);
    }

    static void Seed(atlas::Connection& connection) {
        InsertCharacter(connection, kCharacterLow, "atlas-low", kExpLow);
        InsertCharacter(connection, kCharacterMid, "atlas-mid", kExpMid);
        InsertCharacter(connection, kCharacterHigh, "atlas-high", kExpHigh);
        // 🔴 1001 is `Rusty Sword`, slot 1, out of shared/datas/item.csv. Since info_generator
        // landed the equip path refuses an item_id the static data does not define (§8.2 layer 3),
        // so an arbitrary number here would make every equip below a refusal.
        InsertItem(connection, static_cast<atlas::CharacterId>(kCharacterMid), kItemHeld, 1001,
                   EquipSlot::None);
    }

    void ClearRedisKeys() {
        RunCommand(atlas::RedisCommand{
            .verb = "DEL",
            .args = {atlas_demo::ExpRankingKey(kServerId),
                     atlas_demo::CharacterCacheKey(kServerId,
                                                   static_cast<atlas::CharacterId>(kCharacterLow)),
                     atlas_demo::CharacterCacheKey(kServerId,
                                                   static_cast<atlas::CharacterId>(kCharacterMid)),
                     atlas_demo::CharacterCacheKey(
                         kServerId, static_cast<atlas::CharacterId>(kCharacterHigh))}});
    }

    // 🔴 The production paths are all asynchronous; a test that asserted from inside a completion
    // would be asserting on an I/O thread, where a failed EXPECT has nowhere to be reported from.
    // Every case below therefore drives the async API and waits here, on the test thread.
    atlas::RedisResult RunCommand(const atlas::RedisCommand& command) {
        auto answered = std::make_shared<std::promise<atlas::RedisResult>>();
        std::future<atlas::RedisResult> pending = answered->get_future();
        runner_->Submit(atlas::Ctx{}, command,
                        [answered](const atlas::Ctx&, const atlas::RedisResult& value) {
                            answered->set_value(value);
                        });
        if (pending.wait_for(atlas::Seconds{5}) != std::future_status::ready) {
            ADD_FAILURE() << "a redis command never completed";
            return atlas::RedisResult{};
        }
        return pending.get();
    }

    std::optional<CachedCharacter> GetCached(UInt64 character_id) {
        auto answered = std::make_shared<std::promise<std::optional<CachedCharacter>>>();
        std::future<std::optional<CachedCharacter>> pending = answered->get_future();
        cache_->Get(atlas::Ctx{}, kServerId, static_cast<atlas::CharacterId>(character_id),
                    [answered](std::optional<CachedCharacter> value) {
                        answered->set_value(std::move(value));
                    });
        if (pending.wait_for(atlas::Seconds{5}) != std::future_status::ready) {
            ADD_FAILURE() << "a cache lookup never completed";
            return std::nullopt;
        }
        return pending.get();
    }

    std::optional<std::vector<RankEntry>> TopRanked(UInt16 count) {
        auto answered = std::make_shared<std::promise<std::optional<std::vector<RankEntry>>>>();
        std::future<std::optional<std::vector<RankEntry>>> pending = answered->get_future();
        ranking_->Top(atlas::Ctx{}, kServerId, count,
                      [answered](bool ok, const std::vector<RankEntry>& entries) {
                          answered->set_value(ok ? std::optional<std::vector<RankEntry>>{entries}
                                                 : std::nullopt);
                      });
        if (pending.wait_for(atlas::Seconds{5}) != std::future_status::ready) {
            ADD_FAILURE() << "a ranking query never completed";
            return std::nullopt;
        }
        return pending.get();
    }

    // Runs the real cold load on the test thread, which is where the ZADD after the commit comes
    // from in the ranking cases below.
    atlas_demo::CharacterSnapshot ColdLoad(UInt64 character_id) {
        atlas_demo::CharacterSnapshot snapshot;
        atlas::Ctx ctx;
        atlas_demo::LoadCharacterOnDbThread(ctx, *probe_, kServerId,
                                            static_cast<atlas::CharacterId>(character_id), snapshot,
                                            ranking_.get());
        return snapshot;
    }

    DbConnectionConfig db_config_{};
    atlas::RedisConnectionConfig redis_config_{};
    std::unique_ptr<atlas::Connection> probe_;
    std::unique_ptr<atlas::IoRunner> io_runner_;
    std::unique_ptr<atlas::RedisConnection> redis_;
    std::unique_ptr<atlas::RedisRunner> runner_;
    std::unique_ptr<atlas_demo::ExpRanking> ranking_;
    std::unique_ptr<atlas_demo::CharacterCache> cache_;
};

// ── Case 1 — the ranking is ordered by the authoritative exp ─────────────────────────────────

TEST_F(GameCacheTest, TheRankingReturnsTheTopCharactersInDescendingExp) {
    // Each load commits and only then records, so after these three the sorted set holds exactly
    // what the database does.
    ASSERT_EQ(ColdLoad(kCharacterLow).result, atlas_demo::LoadResult::Ok);
    ASSERT_EQ(ColdLoad(kCharacterHigh).result, atlas_demo::LoadResult::Ok);
    ASSERT_EQ(ColdLoad(kCharacterMid).result, atlas_demo::LoadResult::Ok);

    const std::optional<std::vector<RankEntry>> top = TopRanked(10);
    ASSERT_TRUE(top.has_value());
    ASSERT_EQ(top->size(), std::size_t{3});

    // 🔴 Descending, and the insertion order above was low, HIGH, mid — so a store that simply
    // replayed what it was given would fail this.
    EXPECT_EQ((*top)[0].character_id, static_cast<atlas::CharacterId>(kCharacterHigh));
    EXPECT_EQ((*top)[0].exp, kExpHigh);
    EXPECT_EQ((*top)[1].character_id, static_cast<atlas::CharacterId>(kCharacterMid));
    EXPECT_EQ((*top)[1].exp, kExpMid);
    EXPECT_EQ((*top)[2].character_id, static_cast<atlas::CharacterId>(kCharacterLow));
    EXPECT_EQ((*top)[2].exp, kExpLow);

    // The count is honoured, so a client cannot make the server produce an arbitrary response.
    const std::optional<std::vector<RankEntry>> two = TopRanked(2);
    ASSERT_TRUE(two.has_value());
    EXPECT_EQ(two->size(), std::size_t{2});
}

// ── Case 2 — miss, then hit, and the stored value survives the round trip ────────────────────

TEST_F(GameCacheTest, TheCacheMissesUntilItIsFilledAndThenReturnsTheSameState) {
    EXPECT_FALSE(GetCached(kCharacterMid).has_value());

    const atlas_demo::CharacterSnapshot loaded = ColdLoad(kCharacterMid);
    ASSERT_EQ(loaded.result, atlas_demo::LoadResult::Ok);
    cache_->Put(atlas::Ctx{}, kServerId,
                CachedCharacter{.character = loaded.character, .items = loaded.items});

    const std::optional<CachedCharacter> hit = GetCached(kCharacterMid);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->character.character_id_, loaded.character.character_id_);
    EXPECT_EQ(hit->character.account_uid_, loaded.character.account_uid_);
    EXPECT_EQ(hit->character.name_, "atlas-mid");
    EXPECT_EQ(hit->character.exp_, kExpMid);
    // The nullable column keeps its presence, not just its value.
    EXPECT_EQ(hit->character.last_login_at_.has_value(),
              loaded.character.last_login_at_.has_value());
    ASSERT_EQ(hit->items.size(), std::size_t{1});
    EXPECT_EQ(hit->items.front().item_uid, kItemHeld);
    EXPECT_EQ(hit->items.front().slot, EquipSlot::None);

    // 🔴 The TTL is on the key, not in a field we can read back — so what is asserted is that the
    // key HAS one. A SET that forgot `EX` would leave the copy behind forever, and nothing else in
    // this suite would notice.
    const atlas::RedisResult ttl = RunCommand(
        atlas::RedisCommand{.verb = "TTL",
                            .args = {atlas_demo::CharacterCacheKey(
                                kServerId, static_cast<atlas::CharacterId>(kCharacterMid))}});
    ASSERT_TRUE(ttl.ok);
    ASSERT_EQ(ttl.values.size(), std::size_t{1});
    ASSERT_TRUE(ttl.values.front().has_value());
    EXPECT_NE(*ttl.values.front(), "-1");
}

// ── Case 3 — 🔴 REDIS IS DOWN AND THE CHARACTER STILL LOADS ─────────────────────────────────
//
// The most important case in this file. A real client, a real socket, a real GAME server — and a
// cache endpoint nothing is listening on. If the answer still comes back Ok then the copy is not
// on the critical path, which is the entire claim §10.2 makes about this layer.

TEST_F(GameCacheTest, ACharacterLoadsFromTheDatabaseWhileRedisIsUnreachable) {
    atlas::RedisConnectionConfig dead = redis_config_;
    dead.port = kDeadRedisPort;

    atlas_demo::GameServer server(TestServerOptions(), db_config_, dead);
    server.Start();

    {
        TestClient client(server.LocalEndpoint());
        client.Send(atlas_demo::kOpCharacterLoadRequest, LoadRequestPayload(kCharacterMid));
        const LoadedCharacter loaded = ParseLoadResponse(client.Receive());

        ASSERT_EQ(loaded.result, static_cast<UInt8>(atlas_demo::LoadResult::Ok));
        EXPECT_EQ(loaded.character_id, kCharacterMid);
        EXPECT_EQ(loaded.name, "atlas-mid");
        EXPECT_EQ(loaded.exp, kExpMid);
        ASSERT_EQ(loaded.items.size(), std::size_t{1});
        EXPECT_EQ(loaded.items.front().item_uid, kItemHeld);

        // 🔴 And the write path still works with the cache gone: the invalidation that follows the
        // commit fails, is logged, and changes nothing about the transaction that already landed.
        client.Send(atlas_demo::kOpEquipRequest, EquipRequestPayload(kItemHeld, EquipSlot::Weapon));
        const atlas::Frame equip_response = client.Receive();
        std::span<const Byte> cursor(equip_response.payload);
        UInt8 code = 0xFF;
        ASSERT_TRUE(atlas::generated::ReadLe(cursor, code));
        EXPECT_EQ(code, static_cast<UInt8>(atlas_demo::EquipResult::Ok));
        EXPECT_EQ(SlotOf(*probe_, kItemHeld), static_cast<UInt8>(EquipSlot::Weapon));

        // 🔴 The ranking, by contrast, CANNOT fall back — there is no database form of it — so it
        // reports the outage instead of drawing an empty leaderboard.
        std::vector<Byte> request;
        atlas::generated::WriteLe(request, UInt16{10});
        client.Send(atlas_demo::kOpRankingRequest, request);
        const atlas::Frame ranking_response = client.Receive();
        ASSERT_EQ(ranking_response.opcode, atlas_demo::kOpRankingResponse);
        std::span<const Byte> ranking_cursor(ranking_response.payload);
        UInt8 ranking_code = 0xFF;
        ASSERT_TRUE(atlas::generated::ReadLe(ranking_cursor, ranking_code));
        EXPECT_EQ(ranking_code, static_cast<UInt8>(atlas_demo::RankingResult::Unavailable));
    }

    ASSERT_TRUE(WaitUntil([&] { return server.LiveSessionCount() == 0; }));
    server.Stop();
}

// ── Case 4 — a write drops the copy, over the real server ───────────────────────────────────

TEST_F(GameCacheTest, AnEquipInvalidatesTheCopySoTheNextLoadSeesTheNewSlot) {
    atlas_demo::GameServer server(TestServerOptions(), db_config_, redis_config_);
    server.Start();

    const std::string key =
        atlas_demo::CharacterCacheKey(kServerId, static_cast<atlas::CharacterId>(kCharacterMid));

    {
        TestClient client(server.LocalEndpoint());

        // Cold: the database answers and the copy is written.
        client.Send(atlas_demo::kOpCharacterLoadRequest, LoadRequestPayload(kCharacterMid));
        ASSERT_EQ(ParseLoadResponse(client.Receive()).result,
                  static_cast<UInt8>(atlas_demo::LoadResult::Ok));
        ASSERT_TRUE(WaitUntil([&] { return GetCached(kCharacterMid).has_value(); }));

        // Warm: the same answer, now served from the copy.
        client.Send(atlas_demo::kOpCharacterLoadRequest, LoadRequestPayload(kCharacterMid));
        const LoadedCharacter warm = ParseLoadResponse(client.Receive());
        ASSERT_EQ(warm.result, static_cast<UInt8>(atlas_demo::LoadResult::Ok));
        ASSERT_EQ(warm.items.size(), std::size_t{1});
        EXPECT_EQ(warm.items.front().slot, EquipSlot::None);

        // The write. 🔴 The copy is now wrong, and the answer is to DROP it — not to patch it.
        // The slot is the WEAPON one because that is the slot item.csv gives item_id 1001; which
        // slot it is has never mattered to this case, only that the copy is contradicted.
        client.Send(atlas_demo::kOpEquipRequest, EquipRequestPayload(kItemHeld, EquipSlot::Weapon));
        const atlas::Frame equip_response = client.Receive();
        std::span<const Byte> cursor(equip_response.payload);
        UInt8 code = 0xFF;
        ASSERT_TRUE(atlas::generated::ReadLe(cursor, code));
        ASSERT_EQ(code, static_cast<UInt8>(atlas_demo::EquipResult::Ok));
        ASSERT_TRUE(WaitUntil([&] { return !GetCached(kCharacterMid).has_value(); }));

        // 🔴 The point of the whole case: the next reader sees the NEW state. A cache that had
        // survived the write would answer EquipSlot::None here and the client would be told a
        // fact the database has already contradicted.
        client.Send(atlas_demo::kOpCharacterLoadRequest, LoadRequestPayload(kCharacterMid));
        const LoadedCharacter refreshed = ParseLoadResponse(client.Receive());
        ASSERT_EQ(refreshed.result, static_cast<UInt8>(atlas_demo::LoadResult::Ok));
        ASSERT_NE(FindItem(refreshed, kItemHeld), nullptr);
        EXPECT_EQ(FindItem(refreshed, kItemHeld)->slot, EquipSlot::Weapon);
    }

    ASSERT_TRUE(WaitUntil([&] { return server.LiveSessionCount() == 0; }));
    server.Stop();
    EXPECT_TRUE(RunCommand(atlas::RedisCommand{.verb = "DEL", .args = {key}}).ok);
}

// ── Case 5 — no commit, no ranking ──────────────────────────────────────────────────────────
//
// 🔴 The database is the source of truth, so a value it never accepted must not appear in the
// copy. The fault injector makes the commit not happen; the committing run at the bottom is what
// keeps the assertion above it from being vacuous.

TEST_F(GameCacheTest, ARankingEntryIsNotWrittenWhenTheTransactionDoesNotCommit) {
    const atlas::RedisCommand score{
        .verb = "ZSCORE",
        .args = {atlas_demo::ExpRankingKey(kServerId), std::to_string(kCharacterHigh)}};

    atlas_demo::CharacterSnapshot snapshot;
    atlas::Ctx ctx;
    EXPECT_THROW(
        atlas_demo::LoadCharacterOnDbThread(
            ctx, *probe_, kServerId, static_cast<atlas::CharacterId>(kCharacterHigh), snapshot,
            ranking_.get(), [] { throw std::runtime_error("injected: the commit never happens"); }),
        std::runtime_error);

    // The scope unwound, so the transaction rolled back (§10) and the load produced nothing.
    EXPECT_EQ(ctx.tx_state, atlas::TxState::RolledBack);
    EXPECT_NE(snapshot.result, atlas_demo::LoadResult::Ok);

    const atlas::RedisResult missing = RunCommand(score);
    ASSERT_TRUE(missing.ok);
    ASSERT_EQ(missing.values.size(), std::size_t{1});
    EXPECT_FALSE(missing.values.front().has_value()) << "the ranking holds an uncommitted value";

    // 🔴 Not vacuous: the same call WITHOUT the injector does write the copy.
    ASSERT_EQ(ColdLoad(kCharacterHigh).result, atlas_demo::LoadResult::Ok);
    const atlas::RedisResult present = RunCommand(score);
    ASSERT_TRUE(present.ok);
    ASSERT_EQ(present.values.size(), std::size_t{1});
    ASSERT_TRUE(present.values.front().has_value());
    EXPECT_EQ(*present.values.front(), std::to_string(kExpHigh));
}

// ── Case 6 — the encoding round trip, with no store involved ─────────────────────────────────

TEST(CharacterCacheCodec, EveryFieldSurvivesTheRoundTripAndGarbageDecodesAsAMiss) {
    CachedCharacter value;
    value.character.server_id_ = kServerId;
    value.character.character_id_ = static_cast<atlas::CharacterId>(kCharacterMid);
    value.character.account_uid_ = static_cast<atlas::AccountId>(kAccountUid);
    value.character.name_ = "atlas-mid";
    value.character.pos_x_ = -12;
    value.character.pos_y_ = 34;
    value.character.level_ = 7;
    value.character.exp_ = kExpMid;
    value.character.created_at_ = NowToWholeSecond();
    value.character.last_login_at_ = std::nullopt;
    value.items.push_back(atlas_demo::Item{
        .item_uid = kItemHeld, .item_id = 21, .stack_count = 3, .slot = EquipSlot::Trinket});

    const std::optional<CachedCharacter> decoded =
        atlas_demo::DecodeCachedCharacter(atlas_demo::EncodeCachedCharacter(value));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->character, value.character);
    EXPECT_EQ(decoded->items, value.items);

    // 🔴 A value this build cannot read is a MISS, never an exception: the database can always
    // answer, so there is nothing here worth failing a request over.
    EXPECT_FALSE(atlas_demo::DecodeCachedCharacter("not an encoded character").has_value());
    EXPECT_FALSE(atlas_demo::DecodeCachedCharacter("").has_value());
}

}  // namespace
