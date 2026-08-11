#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "atlas/config/secret_config.h"
#include "atlas/core/ctx.h"
#include "atlas/core/error.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/db/connection_pool.h"
#include "atlas/db/idempotency.h"
#include "atlas/db/prepared_statement.h"
#include "atlas/db/transaction.h"
#include "generated/db/character_items_row.h"
#include "generated/db/characters_row.h"
#include "tests/optional_assert.h"

// Fault-injection suite for atomicity across a server boundary (architecture-design.md §10.4).
//
// 🔴 WHAT THIS SUITE CLAIMS, AND WHAT IT DOES NOT.
// It claims that a request crossing a boundary is executed at most once and that its outcome can be
// recovered afterwards. It does NOT claim a distributed transaction: there is no 2PC and no Saga
// coordinator, nothing here is atomic across two databases, and network partitions are out of
// scope. The idempotency record is in process memory, so it does not survive a restart — case 4
// below asserts that loss instead of hiding it.
//
// 🔴 WHY THIS IS THE BOUNDARY TEST EVEN WITH ONE PROCESS. The property under test is not "three
// processes exchanged packets", it is the state machine Received -> Persisted -> Responded plus a
// key the sender chose. None of it depends on where the sender is: the same handler holds whether
// the request arrived from a client through FE or, in Phase 3, from another server through Relay
// (§5.2). What a real Relay would add is transport, not a different set of states — and the honest
// version of that argument is this file plus the limitations named above.
//
// 🔴 With no database reachable the suite SKIPS, for the reason db_runtime_test.cpp states: a green
// run that proved nothing is worse than a red one. The two cases that need no database are plain
// TESTs outside the fixture and always run.

namespace {

using atlas::Ctx;
using atlas::DbConnectionConfig;
using atlas::DbRow;
using atlas::DbValue;
using atlas::Duration;
using atlas::IdempotencyStore;
using atlas::Int64;
using atlas::RequestAdmission;
using atlas::RequestAdmissionResult;
using atlas::RequestKey;
using atlas::RequestKeyValue;
using atlas::RequestState;
using atlas::SysTime;
using atlas::TxState;
using atlas::UInt16;
using atlas::UInt64;

// A server_id no other suite and no seeded data uses, so every row this file writes is its own.
// db_runtime_test.cpp owns 30001.
constexpr UInt64 kTxServerId = 30002;
constexpr UInt64 kTxCharacterId = 5150;
constexpr UInt64 kTxAccountUid = 9002;
constexpr UInt64 kBaseExp = 1000;

// How long a caller waits for an in-flight duplicate to finish before being told "in progress".
constexpr atlas::Seconds kWaitBudget{5};
// Comfortably longer than any request here; the TTL is a memory bound, not an execution timeout.
constexpr atlas::Seconds kRecordTtl{60};

// Fixed text with placeholders, like everything else in this layer — nothing is concatenated.
constexpr std::string_view kDeleteCharactersSql = "DELETE FROM `characters` WHERE `server_id` = ?";
constexpr std::string_view kDeleteRequestRowsSql =
    "DELETE FROM `character_items` WHERE `server_id` = ?";
constexpr std::string_view kSelectRequestRowsSql =
    "SELECT `item_uid` FROM `character_items` WHERE `server_id` = ? AND `character_id` = ?";

std::optional<DbConnectionConfig> LoadTxConfig() {
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
    // See game_equip_test.cpp — the suite connects on the same TLS terms as the server.
    config.tls_no_verify = secrets.db_tls_no_verify;
#if defined(ATLAS_MARIADB_PLUGIN_DIR)
    config.plugin_directory = ATLAS_MARIADB_PLUGIN_DIR;
#endif
    return config;
}

// DATETIME in the schema has no fractional part, so a value with microseconds would not survive the
// round trip and the comparison would be testing the schema rather than the layer.
SysTime NowToWholeSecond() {
    return std::chrono::floor<std::chrono::seconds>(atlas::SysClock::now());
}

// ── The durable state the requests operate on ────────────────────────────────────────────────
// One `characters` row carrying a counter, plus one `character_items` row per completed request
// whose PRIMARY KEY IS THE REQUEST KEY. That second row is not decoration: it is the only durable
// evidence that a given request already committed, and case 4 recovers from exactly it.

DbRow ReadCharacterRow(atlas::Connection& connection) {
    const std::array<DbValue, 2> parameters{DbValue{kTxServerId}, DbValue{kTxCharacterId}};
    std::vector<DbRow> rows =
        connection.Prepare(atlas::generated::kCharactersSelectByPkSql).Query(parameters);
    if (rows.empty()) {
        throw std::runtime_error("the fixture row is missing");
    }
    return rows.front();
}

UInt64 ReadExp(atlas::Connection& connection) {
    return std::get<UInt64>(ReadCharacterRow(connection)[atlas::generated::kCharactersColExp]);
}

void WriteExp(atlas::Connection& connection, UInt64 exp) {
    DbRow row = ReadCharacterRow(connection);
    row[atlas::generated::kCharactersColExp] = DbValue{exp};

    // 🔴 The key columns bind LAST in an UPDATE, which is precisely why the generated binding array
    // is walked instead of the column order being assumed.
    std::vector<DbValue> parameters;
    parameters.reserve(atlas::generated::kCharactersUpdateByPkBinding.size());
    for (const std::size_t column : atlas::generated::kCharactersUpdateByPkBinding) {
        parameters.push_back(row[column]);
    }
    connection.Prepare(atlas::generated::kCharactersUpdateByPkSql).Execute(parameters);
}

void InsertRequestRow(atlas::Connection& connection, RequestKey key) {
    const std::array<DbValue, 6> parameters{
        DbValue{kTxServerId}, DbValue{kTxCharacterId}, DbValue{RequestKeyValue(key)},
        DbValue{UInt64{1}},   DbValue{UInt64{1}},      DbValue{UInt64{0}}};
    connection.Prepare(atlas::generated::kCharacterItemsInsertSql).Execute(parameters);
}

void DeleteRequestRow(atlas::Connection& connection, RequestKey key) {
    const std::array<DbValue, 3> parameters{DbValue{kTxServerId}, DbValue{kTxCharacterId},
                                            DbValue{RequestKeyValue(key)}};
    connection.Prepare(atlas::generated::kCharacterItemsDeleteByPkSql).Execute(parameters);
}

bool RequestRowExists(atlas::Connection& connection, RequestKey key) {
    const std::array<DbValue, 3> parameters{DbValue{kTxServerId}, DbValue{kTxCharacterId},
                                            DbValue{RequestKeyValue(key)}};
    return !connection.Prepare(atlas::generated::kCharacterItemsSelectByPkSql)
                .Query(parameters)
                .empty();
}

std::size_t CountRequestRows(atlas::Connection& connection) {
    const std::array<DbValue, 2> parameters{DbValue{kTxServerId}, DbValue{kTxCharacterId}};
    return connection.Prepare(kSelectRequestRowsSql).Query(parameters).size();
}

std::string BuildResponse(UInt64 exp_after) { return std::format("applied:exp={}", exp_after); }

// ── The unit under test ──────────────────────────────────────────────────────────────────────

// One server handling labelled requests at most once.
class RequestServer {
public:
    struct Hooks {
        // Runs inside the transaction, before the commit. The latch the concurrency cases need.
        std::function<void()> in_transaction{};
        // The post-commit external side effect. Throwing means it failed, which is what arms the
        // compensation path.
        std::function<void()> side_effect{};
        // 🔴 The injection point for "the response was lost" and "the process died here": the
        // commit is durable and the record says Persisted, but the client is never told.
        bool stop_before_response{false};
    };

    struct Outcome {
        RequestAdmission admission{RequestAdmission::Started};
        std::string response{};
    };

    RequestServer(const DbConnectionConfig& config, Duration record_ttl)
        : pool_(config, 2), store_(record_ttl) {}

    IdempotencyStore& Store() noexcept { return store_; }
    UInt64 Executions() const noexcept { return executions_.load(); }

    Outcome Handle(Ctx& ctx, UInt64 exp_delta, const Hooks& hooks) {
        const RequestAdmissionResult admitted = store_.Begin(ctx, kWaitBudget);
        if (admitted.admission != RequestAdmission::Started) {
            // 🔴 The recorded result, byte for byte. No second execution, no second commit.
            return {.admission = admitted.admission, .response = admitted.result};
        }

        UInt64 exp_after = 0;
        {
            atlas::PooledConnection lease = Lease();
            try {
                Transact(ctx, *lease, exp_delta, hooks, exp_after);
            } catch (...) {
                // The transaction rolled back, so the request did not happen and the key must be
                // free again — otherwise a retransmit would be replayed a result nobody produced.
                store_.Abandon(ctx);
                throw;
            }
        }
        executions_.fetch_add(1);

        // 🔴 AFTER THE COMMIT, NEVER BEFORE. The store rejects this call while the transaction is
        // still open (db/idempotency.h), so the ordering is enforced rather than remembered.
        std::string response = BuildResponse(exp_after);
        store_.MarkPersisted(ctx, response);

        if (hooks.side_effect) {
            // Past the commit, rollback no longer exists — the only way back is the inverse write.
            atlas::PostCommitGuard undo([&] {
                Compensate(ctx, exp_delta);
                store_.Abandon(ctx);
            });
            hooks.side_effect();
            undo.Release();
        }

        if (hooks.stop_before_response) {
            return {.admission = RequestAdmission::Started, .response = std::move(response)};
        }
        store_.MarkResponded(ctx);
        return {.admission = RequestAdmission::Started, .response = std::move(response)};
    }

    // Rebuilds the record from DURABLE state after a restart. Returns false when nothing was
    // committed for this key, in which case a retransmit legitimately executes from scratch.
    bool RecoverFromDurableState(Ctx& ctx) {
        atlas::PooledConnection lease = Lease();
        if (!RequestRowExists(*lease, ctx.request_key)) {
            return false;
        }
        // 🔴 Reconstructed, not replayed: the response bytes died with the process. This is
        // faithful only because the durable state still determines the answer — a result that
        // depended on anything else about that process would not come back.
        store_.Seed(ctx, BuildResponse(ReadExp(*lease)));
        return true;
    }

private:
    // 🔴 Returns the connection itself, not an optional of one: an exhausted pool throws right
    // here, so every caller past this point holds a lease that exists. Handing back an optional
    // made each call site deref something already known to be engaged — `**lease` — which reads as
    // if emptiness were still possible and cost a clang-tidy report at each one.
    atlas::PooledConnection Lease() {
        std::optional<atlas::PooledConnection> lease = pool_.Acquire(atlas::Seconds{5});
        if (!lease.has_value()) {
            throw std::runtime_error("the connection pool was exhausted");
        }
        return std::move(*lease);
    }

    void Transact(Ctx& ctx, atlas::Connection& connection, UInt64 exp_delta, const Hooks& hooks,
                  UInt64& exp_after) {
        atlas::Transaction transaction(connection, ctx);
        exp_after = ReadExp(connection) + exp_delta;
        WriteExp(connection, exp_after);
        // Two tables, one transaction: the counter and the durable evidence of the request either
        // both land or neither does.
        InsertRequestRow(connection, ctx.request_key);
        if (hooks.in_transaction) {
            hooks.in_transaction();
        }
        transaction.Commit();
    }

    void Compensate(const Ctx& ctx, UInt64 exp_delta) {
        atlas::PooledConnection lease = Lease();
        // Its own ledger: the original ctx correctly says Committed, and overwriting that with the
        // compensation's own transaction state would erase the fact being compensated for.
        Ctx compensation_ctx;
        compensation_ctx.trace_id = ctx.trace_id;
        compensation_ctx.request_key = ctx.request_key;

        atlas::Transaction transaction(*lease, compensation_ctx);
        WriteExp(*lease, ReadExp(*lease) - exp_delta);
        DeleteRequestRow(*lease, ctx.request_key);
        transaction.Commit();
    }

    atlas::ConnectionPool pool_;
    IdempotencyStore store_;
    std::atomic<UInt64> executions_{0};
};

class TxAtomicityTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::optional<DbConnectionConfig> loaded = LoadTxConfig();
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
        } catch (const std::exception& ex) {
            GTEST_SKIP() << "SKIPPED: ATLAS_DB_HOST is set but the database refused the "
                            "connection: "
                         << ex.what();
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
        const std::array<DbValue, 1> parameters{DbValue{kTxServerId}};
        connection.Prepare(kDeleteRequestRowsSql).Execute(parameters);
        connection.Prepare(kDeleteCharactersSql).Execute(parameters);
    }

    static void Seed(atlas::Connection& connection) {
        const std::array<DbValue, 10> parameters{
            DbValue{kTxServerId},        DbValue{kTxCharacterId},
            DbValue{kTxAccountUid},      DbValue{std::string{"tx-atomicity"}},
            DbValue{Int64{0}},           DbValue{Int64{0}},
            DbValue{UInt64{1}},          DbValue{kBaseExp},
            DbValue{NowToWholeSecond()}, DbValue{std::monostate{}}};
        connection.Prepare(atlas::generated::kCharactersInsertSql).Execute(parameters);
    }

    DbConnectionConfig config_{};
    std::unique_ptr<atlas::Connection> probe_;
};

// ── Case 1 — the response was lost ───────────────────────────────────────────────────────────
TEST_F(TxAtomicityTest, LostResponseIsReplayedFromTheRecordWithoutASecondExecution) {
    RequestServer server(config_, kRecordTtl);
    const RequestKey key{0xA1};

    Ctx first;
    first.trace_id = 5101;
    first.request_key = key;
    RequestServer::Hooks lose_it;
    lose_it.stop_before_response = true;
    const RequestServer::Outcome sent = server.Handle(first, 50, lose_it);

    // The commit landed and the record says so; the client heard nothing.
    EXPECT_EQ(first.tx_state, TxState::Committed);
    EXPECT_EQ(first.request_state, RequestState::Persisted);
    EXPECT_EQ(server.Executions(), 1U);

    // 🔴 The retransmit is INDISTINGUISHABLE from a fresh duplicate at this end — a different
    // trace id, a different packet, the same labelled request. The key is what settles it.
    Ctx again;
    again.trace_id = 5102;
    again.request_key = key;
    const RequestServer::Outcome replayed = server.Handle(again, 50, RequestServer::Hooks{});

    EXPECT_EQ(replayed.admission, RequestAdmission::Replay);
    EXPECT_EQ(replayed.response, sent.response);
    EXPECT_EQ(server.Executions(), 1U);

    // One application of the delta, read on a connection that never saw the transaction.
    atlas::Connection reader(config_);
    EXPECT_EQ(ReadExp(reader), kBaseExp + 50);
    EXPECT_EQ(CountRequestRows(reader), 1U);
}

// ── Case 2 — two copies of one request arrive at the same time ───────────────────────────────
TEST_F(TxAtomicityTest, SimultaneousDuplicatesExecuteOnceAndAnswerIdentically) {
    RequestServer server(config_, kRecordTtl);
    const RequestKey key{0xB2};

    std::atomic<int> at_the_gate{0};
    std::atomic<bool> threw{false};
    std::array<RequestServer::Outcome, 2> outcomes;
    std::array<Ctx, 2> contexts;
    contexts[0].trace_id = 5201;
    contexts[1].trace_id = 5202;
    contexts[0].request_key = key;
    contexts[1].request_key = key;

    // Caught rather than allowed to escape: an exception leaving a std::thread terminates the whole
    // process, which would take the rest of the suite with it and report nothing useful.
    auto arrive = [&](std::size_t index) {
        at_the_gate.fetch_add(1);
        while (at_the_gate.load() < 2) {
            std::this_thread::yield();
        }
        try {
            outcomes[index] = server.Handle(contexts[index], 70, RequestServer::Hooks{});
        } catch (...) {
            threw.store(true);
        }
    };

    std::thread left(arrive, 0);
    std::thread right(arrive, 1);
    left.join();
    right.join();

    ASSERT_FALSE(threw.load());
    EXPECT_EQ(server.Executions(), 1U);
    // Exactly one owner, and the loser was answered rather than refused: this is also the "wait
    // until it finishes" half of the timeout case — the loser blocked inside Begin until the owner
    // reached Persisted and then took its result.
    const int started = static_cast<int>(outcomes[0].admission == RequestAdmission::Started) +
                        static_cast<int>(outcomes[1].admission == RequestAdmission::Started);
    EXPECT_EQ(started, 1);
    EXPECT_EQ(outcomes[0].response, outcomes[1].response);
    EXPECT_FALSE(outcomes[0].response.empty());

    atlas::Connection reader(config_);
    EXPECT_EQ(ReadExp(reader), kBaseExp + 70);
    EXPECT_EQ(CountRequestRows(reader), 1U);
}

// ── Case 3 — a timeout retransmit lands while the first copy is still running ────────────────
TEST_F(TxAtomicityTest, RetransmitDuringExecutionIsToldInProgressAndNeverRunsASecondCopy) {
    RequestServer server(config_, kRecordTtl);
    const RequestKey key{0xC3};

    std::atomic<bool> inside{false};
    std::atomic<bool> release{false};
    std::atomic<bool> threw{false};

    Ctx owner;
    owner.trace_id = 5301;
    owner.request_key = key;
    RequestServer::Outcome owner_outcome;

    RequestServer::Hooks hold;
    hold.in_transaction = [&] {
        inside.store(true);
        while (!release.load()) {
            std::this_thread::yield();
        }
    };

    std::thread worker([&] {
        try {
            owner_outcome = server.Handle(owner, 90, hold);
        } catch (...) {
            threw.store(true);
        }
        // Whatever happened, stop the main thread from spinning forever waiting to be let in.
        inside.store(true);
    });
    while (!inside.load()) {
        std::this_thread::yield();
    }

    // 🔴 Nothing between here and the join asserts: a failed assertion returns from the test body,
    // and returning while the worker is still parked would leave a joinable thread and terminate.
    // The observations are taken now and checked after the join.

    // The retransmit, asking with a short budget: "is it done yet?" rather than "wait for it".
    Ctx probing;
    probing.trace_id = 5302;
    probing.request_key = key;
    const RequestAdmissionResult probe = server.Store().Begin(probing, atlas::Millis{100});
    // Nothing committed while the owner is still inside its transaction.
    const UInt64 executions_while_held = server.Executions();

    release.store(true);
    worker.join();

    ASSERT_FALSE(threw.load());
    EXPECT_EQ(probe.admission, RequestAdmission::InProgress);
    EXPECT_EQ(probe.state, RequestState::Received);
    EXPECT_TRUE(probe.result.empty());
    EXPECT_EQ(executions_while_held, 0U);
    ASSERT_EQ(server.Executions(), 1U);

    // The same retransmit once the owner finished: answered from the record, still one execution.
    Ctx later;
    later.trace_id = 5303;
    later.request_key = key;
    const RequestServer::Outcome answered = server.Handle(later, 90, RequestServer::Hooks{});
    EXPECT_EQ(answered.admission, RequestAdmission::Replay);
    EXPECT_EQ(answered.response, owner_outcome.response);
    EXPECT_EQ(server.Executions(), 1U);

    atlas::Connection reader(config_);
    EXPECT_EQ(ReadExp(reader), kBaseExp + 90);
    EXPECT_EQ(CountRequestRows(reader), 1U);
}

// ── Case 4 — the process dies between the commit and the response ────────────────────────────
//
// 🔴 READ THE SCOPE BEFORE READING THE ASSERTIONS — two things are involved and only one of them
// is durable.
//
//   * The COMMITTED ROWS are genuinely durable. MySQL accepted them; they are still there.
//   * The IDEMPOTENCY RECORD IS NOT. It is an std::unordered_map in this process
//     (db/idempotency.h), so a restart loses it, and this test does not pretend otherwise.
//
// SIMULATED: the loss of process memory. The store's owner is destroyed and a fresh one is built
// against the same database. For an in-memory store that is not a weaker stand-in for a real
// restart — an empty map is exactly what a real restart leaves. The process is genuinely not
// killed (a gtest case cannot exec itself and come back), so signal handling, connection teardown
// and half-written sockets are NOT exercised here.
//
// 🔴 NOT PROVEN: that the at-most-once promise survives a crash. It does not, and the first
// assertion below states that in the form of a failing lookup. What IS proven is that the state
// machine resumes from durable state: this request wrote a row whose PRIMARY KEY IS THE REQUEST
// KEY, so the row's presence is durable evidence that the request already committed, and recovery
// reads that instead of re-executing. A request whose effect leaves no such trace cannot be
// recovered this way at all — with an in-memory store it would simply run twice. The production
// answer to both is the same one db/idempotency.h names: the record belongs in a table.
TEST_F(TxAtomicityTest, CrashAfterCommitResumesFromDurableStateAndLosesTheInMemoryRecord) {
    const RequestKey key{0xD4};
    std::string response_never_sent;

    {
        RequestServer dying(config_, kRecordTtl);
        Ctx ctx;
        ctx.trace_id = 5401;
        ctx.request_key = key;
        RequestServer::Hooks die_here;
        die_here.stop_before_response = true;

        response_never_sent = dying.Handle(ctx, 120, die_here).response;
        EXPECT_EQ(ctx.tx_state, TxState::Committed);
        EXPECT_EQ(ctx.request_state, RequestState::Persisted);

        const std::optional<RequestState> recorded = dying.Store().Find(key);
        ATLAS_ASSERT_HAS_VALUE(recorded);
        EXPECT_EQ(*recorded, RequestState::Persisted);
    }  // ← the process dies here

    RequestServer restarted(config_, kRecordTtl);

    // The half that did NOT survive, asserted rather than glossed over.
    EXPECT_FALSE(restarted.Store().Find(key).has_value());
    EXPECT_EQ(restarted.Store().Size(), 0U);

    // The half that did: the committed rows, and the invariant they carry.
    atlas::Connection reader(config_);
    EXPECT_EQ(ReadExp(reader), kBaseExp + 120);
    EXPECT_EQ(CountRequestRows(reader), 1U);

    // Resuming. The retransmit reaches the restarted process, recovery finds the durable evidence
    // and seeds Persisted, and the handler then answers from the record without executing.
    Ctx retransmit;
    retransmit.trace_id = 5402;
    retransmit.request_key = key;
    ASSERT_TRUE(restarted.RecoverFromDurableState(retransmit));
    EXPECT_EQ(retransmit.request_state, RequestState::Persisted);

    const RequestServer::Outcome answered =
        restarted.Handle(retransmit, 120, RequestServer::Hooks{});
    EXPECT_EQ(answered.admission, RequestAdmission::Replay);
    EXPECT_EQ(answered.response, response_never_sent);
    EXPECT_EQ(restarted.Executions(), 0U);

    // Exactly one application of the delta survived the whole thing.
    EXPECT_EQ(ReadExp(reader), kBaseExp + 120);
    EXPECT_EQ(CountRequestRows(reader), 1U);
}

// ── The compensation path — a post-commit step failed ────────────────────────────────────────
TEST_F(TxAtomicityTest, PostCommitSideEffectFailureIsUndoneByAnInverseWrite) {
    RequestServer server(config_, kRecordTtl);
    const RequestKey key{0xE5};

    Ctx ctx;
    ctx.trace_id = 5501;
    ctx.request_key = key;
    RequestServer::Hooks failing;
    failing.side_effect = [] {
        throw std::runtime_error("the downstream step refused the committed result");
    };

    // 🔴 The commit already succeeded, so nothing is rolled back here. What runs is a SECOND
    // committed transaction writing the inverse — and between the two, the first result really was
    // visible. Compensation buys the end state back, not the window.
    EXPECT_THROW(server.Handle(ctx, 200, failing), std::runtime_error);
    EXPECT_EQ(ctx.tx_state, TxState::Committed);
    EXPECT_EQ(server.Executions(), 1U);

    atlas::Connection reader(config_);
    EXPECT_EQ(ReadExp(reader), kBaseExp);
    EXPECT_EQ(CountRequestRows(reader), 0U);

    // The request did not, in the end, happen, so the key is free and a retransmit really executes
    // — replaying a result that was undone would be worse than running again.
    EXPECT_FALSE(server.Store().Find(key).has_value());
    Ctx retry;
    retry.trace_id = 5502;
    retry.request_key = key;
    const RequestServer::Outcome retried = server.Handle(retry, 200, RequestServer::Hooks{});
    EXPECT_EQ(retried.admission, RequestAdmission::Started);
    EXPECT_EQ(server.Executions(), 2U);
    EXPECT_EQ(ReadExp(reader), kBaseExp + 200);
    EXPECT_EQ(CountRequestRows(reader), 1U);
}

// ── Ordering and TTL — no database involved, so these always run ─────────────────────────────

TEST(IdempotencyOrderingTest, RecordingAResultWhileTheTransactionIsStillOpenIsRejected) {
    IdempotencyStore store(kRecordTtl);
    Ctx ctx;
    ctx.request_key = RequestKey{0xF1};
    ASSERT_EQ(store.Begin(ctx, atlas::Millis{0}).admission, RequestAdmission::Started);

    // 🔴 This is "respond before the commit" caught mechanically. It is the optimisation someone
    // always proposes, and the reason it is wrong is that the client would then hold a number the
    // database may never accept.
    ctx.tx_state = TxState::Active;
    EXPECT_THROW(store.MarkPersisted(ctx, "premature"), atlas::Exception);

    ctx.tx_state = TxState::Committed;
    store.MarkPersisted(ctx, "after the commit");
    EXPECT_EQ(ctx.request_state, RequestState::Persisted);
}

TEST(IdempotencyOrderingTest, TheStateMachineCannotSkipPersisted) {
    IdempotencyStore store(kRecordTtl);
    Ctx ctx;
    ctx.request_key = RequestKey{0xF2};
    ASSERT_EQ(store.Begin(ctx, atlas::Millis{0}).admission, RequestAdmission::Started);

    EXPECT_THROW(store.MarkResponded(ctx), atlas::Exception);

    ctx.tx_state = TxState::Committed;
    store.MarkPersisted(ctx, "result");
    store.MarkResponded(ctx);
    EXPECT_EQ(ctx.request_state, RequestState::Responded);
}

TEST(IdempotencyOrderingTest, AnUnlabelledRequestIsRefusedRatherThanSharingOneKey) {
    IdempotencyStore store(kRecordTtl);
    Ctx ctx;  // request_key left unset
    EXPECT_THROW(store.Begin(ctx, atlas::Millis{0}), atlas::Exception);
}

TEST(IdempotencyOrderingTest, ExpiredRecordsAreSweptAndFreeTheirKey) {
    IdempotencyStore store(atlas::Millis{20});
    Ctx ctx;
    ctx.request_key = RequestKey{0xF3};
    ASSERT_EQ(store.Begin(ctx, atlas::Millis{0}).admission, RequestAdmission::Started);
    ctx.tx_state = TxState::Committed;
    store.MarkPersisted(ctx, "result");
    ASSERT_EQ(store.Size(), 1U);

    std::this_thread::sleep_for(atlas::Millis{60});

    EXPECT_FALSE(store.Find(RequestKey{0xF3}).has_value());
    EXPECT_EQ(store.PurgeExpired(), 1U);
    EXPECT_EQ(store.Size(), 0U);
}

}  // namespace
