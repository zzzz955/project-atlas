#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
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
#include "atlas/core/ids.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/db/connection_pool.h"
#include "atlas/db/db_runner.h"
#include "atlas/db/prepared_statement.h"
#include "atlas/db/transaction.h"
#include "atlas/net/io_runner.h"
#include "atlas/net/net_types.h"
#include "generated/db/characters_row.h"

// Integration suite for the ORM runtime (architecture-design.md §10.1). It talks to a REAL MySQL —
// `docker compose --env-file server/.env up -d mysql` — because the things under test are exactly
// the ones a fake cannot have: prepared-statement reuse, transaction visibility, and a driver that
// blocks the calling thread.
//
// 🔴 With no database reachable the suite SKIPS and says so. It never passes quietly: a green run
// that proved nothing is worse than a red one, because it retires the question. ctest reports these
// as "Skipped" via the SKIP_REGULAR_EXPRESSION set in tests/CMakeLists.txt.
//
// 🔴 This file is where the generated statements (server/generated/db) meet the runtime. atlas/db
// itself may not include them — core-purity (§15.4) forbids server/atlas/** from reaching into
// server/generated/**, and the row headers carry demo-game vocabulary — so the runtime stays
// generic and the row mapping lives above the core. A test is above the core.

namespace {

using atlas::CharacterId;
using atlas::Ctx;
using atlas::DbConnectionConfig;
using atlas::DbRow;
using atlas::DbValue;
using atlas::Int64;
using atlas::SysTime;
using atlas::TxState;
using atlas::UInt16;
using atlas::UInt64;

// A server_id no other suite and no seeded data uses, so every row this file writes is its own.
constexpr UInt64 kTestServerId = 30001;

// Fixed text with one placeholder, like everything else in this layer — nothing is concatenated.
constexpr std::string_view kDeleteAllForServerSql =
    "DELETE FROM `characters` WHERE `server_id` = ?";

std::optional<DbConnectionConfig> LoadConfig() {
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
    // Baked in at configure time, the same way the config suite is handed the committed server.ini
    // path. Deriving it from the working directory would make the suite pass under ctest and skip
    // everywhere else — and the skip would blame the credentials (see the note in connection.h).
    config.plugin_directory = ATLAS_MARIADB_PLUGIN_DIR;
#endif
    return config;
}

// Truncated to whole seconds: schema.sql declares DATETIME with no fractional part, so a value with
// microseconds would not survive the round trip and the comparison would be testing the schema
// rather than the layer.
SysTime NowToSecond() { return std::chrono::floor<std::chrono::seconds>(atlas::SysClock::now()); }

std::vector<DbValue> MakeCharacterRow(UInt64 character_id, const std::string& name,
                                      SysTime created_at) {
    // Binding order is kCharactersInsertBinding — the generated declaration order.
    return {DbValue{kTestServerId}, DbValue{character_id},
            DbValue{UInt64{9001}},  DbValue{name},
            DbValue{Int64{12}},     DbValue{Int64{-34}},
            DbValue{UInt64{7}},     DbValue{UInt64{4242}},
            DbValue{created_at},    DbValue{std::monostate{}}};
}

// 🔴 How a connection dies for real: the SERVER closes it (a restart, or wait_timeout expiring on
// an idle one) while the pool still believes it owns a live socket. `KILL` from a second connection
// reproduces exactly that, without stopping the container the whole suite shares. Both statements
// are fixed text with a placeholder, like everything else in this layer.
constexpr std::string_view kConnectionIdSql = "SELECT CONNECTION_ID()";
constexpr std::string_view kKillConnectionSql = "KILL ?";

UInt64 DriverConnectionId(atlas::Connection& connection) {
    const std::vector<DbValue> no_parameters;
    const std::vector<DbRow> rows = connection.Prepare(kConnectionIdSql).Query(no_parameters);
    return rows.empty() ? UInt64{0} : std::get<UInt64>(rows[0][0]);
}

void KillDriverConnection(atlas::Connection& killer, UInt64 connection_id) {
    const std::vector<DbValue> parameters{DbValue{connection_id}};
    killer.Prepare(kKillConnectionSql).Execute(parameters);
}

class DbRuntimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::optional<DbConnectionConfig> loaded = LoadConfig();
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
    }

    void TearDown() override {
        if (probe_) {
            Cleanup(*probe_);
        }
    }

    static void Cleanup(atlas::Connection& connection) {
        const std::array<DbValue, 1> parameters{DbValue{kTestServerId}};
        connection.Prepare(kDeleteAllForServerSql).Execute(parameters);
    }

    static std::vector<DbRow> SelectByPk(atlas::Connection& connection, UInt64 character_id) {
        const std::array<DbValue, 2> parameters{DbValue{kTestServerId}, DbValue{character_id}};
        return connection.Prepare(atlas::generated::kCharactersSelectByPkSql).Query(parameters);
    }

    DbConnectionConfig config_{};
    std::unique_ptr<atlas::Connection> probe_;
};

TEST_F(DbRuntimeTest, PoolLeasesUpToItsSizeAndThenMakesTheNextCallerWait) {
    atlas::ConnectionPool pool(config_, 2);
    ASSERT_EQ(pool.Size(), 2U);
    ASSERT_EQ(pool.Available(), 2U);

    std::optional<atlas::PooledConnection> first = pool.Acquire(atlas::Seconds{2});
    std::optional<atlas::PooledConnection> second = pool.Acquire(atlas::Seconds{2});
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(pool.Available(), 0U);

    // The pool is empty, so this one cannot be served.
    EXPECT_FALSE(pool.Acquire(atlas::Millis{100}).has_value());

    // Returning a lease is what unblocks it, and the RAII scope is what returns it.
    first.reset();
    EXPECT_EQ(pool.Available(), 1U);
    std::optional<atlas::PooledConnection> third = pool.Acquire(atlas::Seconds{2});
    EXPECT_TRUE(third.has_value());
}

TEST_F(DbRuntimeTest, ExhaustedPoolFailsAfterTheTimeoutInsteadOfWaitingForever) {
    atlas::ConnectionPool pool(config_, 1);
    const std::optional<atlas::PooledConnection> held = pool.Acquire(atlas::Seconds{2});
    ASSERT_TRUE(held.has_value());

    const atlas::TimePoint started = atlas::Clock::now();
    const std::optional<atlas::PooledConnection> denied = pool.Acquire(atlas::Millis{250});
    const atlas::Duration elapsed = atlas::Clock::now() - started;

    EXPECT_FALSE(denied.has_value());
    // It really waited, and it really stopped.
    EXPECT_GE(elapsed, atlas::Millis{200});
    EXPECT_LT(elapsed, atlas::Seconds{5});
}

TEST_F(DbRuntimeTest, ACachedStatementIsNotPreparedTwice) {
    atlas::Connection connection(config_);
    EXPECT_EQ(connection.PrepareCount(), 0U);

    atlas::PreparedStatement& first =
        connection.Prepare(atlas::generated::kCharactersSelectByPkSql);
    EXPECT_EQ(connection.PrepareCount(), 1U);

    atlas::PreparedStatement& second =
        connection.Prepare(atlas::generated::kCharactersSelectByPkSql);
    // Same object, and the counter did not move: zero re-preparations.
    EXPECT_EQ(&first, &second);
    EXPECT_EQ(connection.PrepareCount(), 1U);

    connection.Prepare(atlas::generated::kCharactersInsertSql);
    EXPECT_EQ(connection.PrepareCount(), 2U);

    // The generated statement text and the generated binding order agree at compile time; this
    // asserts the server agrees too.
    EXPECT_EQ(first.ParameterCount(), atlas::generated::kCharactersSelectByPkBinding.size());
}

TEST_F(DbRuntimeTest, CommittedTransactionIsVisibleAfterwards) {
    atlas::Connection writer(config_);
    Ctx ctx;
    ctx.trace_id = 4711;

    const SysTime created_at = NowToSecond();
    const std::vector<DbValue> row = MakeCharacterRow(101, "commit-case", created_at);
    {
        atlas::Transaction transaction(writer, ctx);
        EXPECT_EQ(ctx.tx_state, TxState::Active);
        EXPECT_EQ(writer.Prepare(atlas::generated::kCharactersInsertSql).Execute(row), 1U);
        transaction.Commit();
    }
    EXPECT_EQ(ctx.tx_state, TxState::Committed);

    // Read on a DIFFERENT connection: a value only the writing session can see is not committed.
    atlas::Connection reader(config_);
    const std::vector<DbRow> found = SelectByPk(reader, 101);
    ASSERT_EQ(found.size(), 1U);
    ASSERT_EQ(found[0].size(), atlas::generated::kCharactersColumnCount);

    EXPECT_EQ(std::get<UInt64>(found[0][atlas::generated::kCharactersColServerId]), kTestServerId);
    EXPECT_EQ(std::get<std::string>(found[0][atlas::generated::kCharactersColName]), "commit-case");
    // A signed column comes back signed, and the negative value proves the unsigned flag is being
    // read from the field rather than assumed.
    EXPECT_EQ(std::get<Int64>(found[0][atlas::generated::kCharactersColPosY]), -34);
    EXPECT_EQ(std::get<SysTime>(found[0][atlas::generated::kCharactersColCreatedAt]), created_at);
    // A NULL column is monostate, not a zero value.
    EXPECT_TRUE(std::holds_alternative<std::monostate>(
        found[0][atlas::generated::kCharactersColLastLoginAt]));
}

TEST_F(DbRuntimeTest, TransactionRollsBackWhenTheScopeUnwindsInsideGuarded) {
    atlas::Connection writer(config_);
    Ctx ctx;
    ctx.trace_id = 4712;
    ctx.character_id = CharacterId{102};

    const std::vector<DbValue> row = MakeCharacterRow(102, "rollback-case", NowToSecond());

    // 🔴 Run it through the real guard, not a bare try/catch: architecture-design.md §10 says the
    // RAII scope and the exception guard are one set, and §11.2b routes every handler through
    // Guarded. What must hold is that the guard swallowing the throw still leaves the ledger
    // saying a transaction was open and got rolled back.
    atlas::Guarded(ctx, [&] {
        atlas::Transaction transaction(writer, ctx);
        writer.Prepare(atlas::generated::kCharactersInsertSql).Execute(row);
        throw std::runtime_error("deliberate failure inside the transaction scope");
    })();

    // Guarded restored the thread_local ledger on exit, so this assertion is only possible because
    // Transaction writes through the CALLER's Ctx (transaction.h explains why).
    EXPECT_EQ(ctx.tx_state, TxState::RolledBack);

    atlas::Connection reader(config_);
    EXPECT_TRUE(SelectByPk(reader, 102).empty());
}

TEST_F(DbRuntimeTest, PoolRevivesAConnectionTheServerClosedUnderneathIt) {
    atlas::ConnectionPool pool(config_, 1);

    UInt64 killed_id = 0;
    {
        std::optional<atlas::PooledConnection> lease = pool.Acquire(atlas::Seconds{2});
        ASSERT_TRUE(lease.has_value());
        killed_id = DriverConnectionId(**lease);
        KillDriverConnection(*probe_, killed_id);
    }  // The dead connection goes back to the pool, exactly as it would in production.

    // 🔴 No process restart between these two lines. That is the entire claim: before this node the
    // pool handed the corpse back out and every request after a MySQL restart failed forever.
    std::optional<atlas::PooledConnection> revived = pool.Acquire(atlas::Seconds{5});
    ASSERT_TRUE(revived.has_value());

    // A different server-side thread id, so it really is a new socket and not the killed one
    // answering; and it serves a real statement, which is the only proof the handle is usable.
    EXPECT_NE(DriverConnectionId(**revived), killed_id);
    const std::vector<DbValue> parameters{DbValue{kTestServerId}};
    EXPECT_NO_THROW((*revived)->Prepare(kDeleteAllForServerSql).Execute(parameters));
}

TEST_F(DbRuntimeTest, ReconnectDropsThePreparedStatementCache) {
    atlas::Connection connection(config_);
    const UInt64 killed_id = DriverConnectionId(connection);
    connection.Prepare(atlas::generated::kCharactersSelectByPkSql);
    const UInt64 prepared_before = connection.PrepareCount();
    ASSERT_EQ(prepared_before, 2U);

    KillDriverConnection(*probe_, killed_id);
    ASSERT_TRUE(connection.EnsureAlive());
    EXPECT_NE(DriverConnectionId(connection), killed_id);

    // 🔴 The evidence is the counter, not a pointer comparison: the old PreparedStatement objects
    // were destroyed along with the cache, so comparing their addresses would be reading freed
    // memory. What matters is that the SAME sql text prepares again — if it had not, the cache
    // would still be holding a MYSQL_STMT that belongs to the socket the server closed, and using
    // it is a use-after-free inside the driver rather than an error anyone gets to see.
    connection.Prepare(atlas::generated::kCharactersSelectByPkSql);
    EXPECT_EQ(connection.PrepareCount(), prepared_before + 2U);
}

TEST_F(DbRuntimeTest, ReconnectCannotInventADatabaseThatIsNotThere) {
    // Same entry point the reconnect takes (Connection::Open), pointed at an endpoint with nothing
    // behind it. When it fails, EnsureAlive returns false and the pool reports its existing
    // unavailable answer — there is no path here that yields rows or a silent success.
    //
    // 🔴 The end-to-end version of this — the database itself going away under a running server —
    // is `docker compose restart mysql`, which a test may not do to the container the rest of the
    // suite is using. It is verified by hand and written down in architecture-design.md §10.
    DbConnectionConfig unreachable = config_;
    unreachable.port = UInt16{1};

    EXPECT_THROW(static_cast<void>(std::make_unique<atlas::Connection>(unreachable)),
                 atlas::DbException);
    EXPECT_THROW(static_cast<void>(std::make_unique<atlas::ConnectionPool>(unreachable, 1)),
                 atlas::DbException);
}

TEST_F(DbRuntimeTest, CtxCrossesIntoTheDbThreadByValue) {
    atlas::ConnectionPool pool(config_, 2);
    atlas::DbRunner runner(pool, 2, atlas::Seconds{2});
    runner.Start();

    Ctx ctx;
    ctx.trace_id = 987654321;
    ctx.character_id = CharacterId{55};

    std::atomic<UInt64> observed_trace{0};
    std::atomic<UInt64> observed_character{0};
    std::atomic<bool> had_connection{false};
    std::atomic<bool> done{false};

    const atlas::SubmitResult submitted = runner.Submit(
        ctx,
        [&](Ctx& job_ctx, atlas::Connection* connection) {
            // Read from the INSTALLED ledger, not from job_ctx: proving the value crossed is only
            // interesting if the thing the log macros read on this thread is the one that crossed.
            observed_trace.store(atlas::CurrentCtx().trace_id);
            observed_character.store(atlas::IdValue(atlas::CurrentCtx().character_id));
            had_connection.store(connection != nullptr);
            job_ctx.tx_state = TxState::Committed;
        },
        [&](const Ctx& done_ctx) {
            // The ledger the job left behind, carried through to the completion.
            EXPECT_EQ(done_ctx.tx_state, TxState::Committed);
            done.store(true);
        });
    ASSERT_EQ(submitted, atlas::SubmitResult::Accepted);

    const atlas::TimePoint deadline = atlas::Clock::now() + atlas::Seconds{5};
    while (!done.load() && atlas::Clock::now() < deadline) {
        std::this_thread::yield();
    }
    runner.Stop();

    EXPECT_TRUE(done.load());
    EXPECT_TRUE(had_connection.load());
    EXPECT_EQ(observed_trace.load(), 987654321U);
    EXPECT_EQ(observed_character.load(), 55U);
    // The caller's own ledger is untouched: the job mutated a copy.
    EXPECT_EQ(ctx.tx_state, TxState::None);
}

TEST_F(DbRuntimeTest, CompletionRunsBackOnTheOriginatingStrand) {
    atlas::IoRunner io_runner(1);
    io_runner.Start();
    atlas::Strand strand = atlas::asio::make_strand(io_runner.Context());

    atlas::ConnectionPool pool(config_, 1);
    atlas::DbRunner runner(pool, 1, atlas::Seconds{2});
    runner.Start();

    std::atomic<bool> job_off_strand{false};
    std::atomic<bool> completion_on_strand{false};
    std::atomic<bool> done{false};

    Ctx ctx;
    ctx.trace_id = 24680;

    const atlas::SubmitResult submitted = runner.Submit(
        ctx,
        [&](Ctx&, atlas::Connection* connection) {
            // 🔴 The blocking half must NOT be on the strand — that is the whole point of the
            // separate pool (§9).
            job_off_strand.store(!strand.running_in_this_thread());
            ASSERT_NE(connection, nullptr);
            const std::array<DbValue, 1> parameters{DbValue{kTestServerId}};
            connection->Prepare(kDeleteAllForServerSql).Execute(parameters);
        },
        [&](const Ctx&) {
            completion_on_strand.store(strand.running_in_this_thread());
            done.store(true);
        },
        [strand](std::function<void()> completion) {
            atlas::asio::post(strand, std::move(completion));
        });
    ASSERT_EQ(submitted, atlas::SubmitResult::Accepted);

    const atlas::TimePoint deadline = atlas::Clock::now() + atlas::Seconds{5};
    while (!done.load() && atlas::Clock::now() < deadline) {
        std::this_thread::yield();
    }
    runner.Stop();
    io_runner.Stop();

    EXPECT_TRUE(done.load());
    EXPECT_TRUE(job_off_strand.load());
    EXPECT_TRUE(completion_on_strand.load());
}

// 🔴 The regression line for load shedding (architecture-design.md §10.8). Without a cap the queue
// grows without bound, and the server keeps committing requests whose clients gave up long ago.
TEST_F(DbRuntimeTest, QueueRefusesPastItsCapAndStopsGrowingThere) {
    atlas::ConnectionPool pool(config_, 1);
    atlas::DbRunner runner(pool, 1, atlas::Seconds{2});
    const std::size_t capacity = runner.QueueCapacity();
    ASSERT_EQ(capacity, atlas::DbRunner::kMaxQueuedJobsPerThread);
    runner.Start();

    // Every job parks until the test releases it, so the queue drains only on command.
    std::atomic<bool> release{false};
    std::atomic<std::size_t> ran{0};
    std::atomic<std::size_t> completed{0};
    std::size_t accepted = 0;
    std::size_t rejected = 0;

    for (std::size_t index = 0; index < capacity + 8; ++index) {
        const atlas::SubmitResult result = runner.Submit(
            Ctx{},
            [&](Ctx&, atlas::Connection*) {
                ran.fetch_add(1);
                while (!release.load()) {
                    std::this_thread::yield();
                }
            },
            [&](const Ctx&) { completed.fetch_add(1); });
        if (result == atlas::SubmitResult::Accepted) {
            ++accepted;
        } else {
            EXPECT_EQ(result, atlas::SubmitResult::Rejected);
            ++rejected;
        }
        // 🔴 The property, checked after EVERY submit rather than once at the end: the queue never
        // grows past the cap. A cap that is only true at the end is a cap that leaked.
        EXPECT_LE(runner.PendingCount(), capacity);
    }

    // The single worker is parked inside the first job, so the queue holds the rest.
    EXPECT_GE(accepted, capacity);
    EXPECT_GT(rejected, 0U);
    EXPECT_EQ(accepted + rejected, capacity + 8);
    EXPECT_EQ(runner.RejectedCount(), static_cast<UInt64>(rejected));

    release.store(true);
    const atlas::TimePoint deadline = atlas::Clock::now() + atlas::Seconds{20};
    while (completed.load() < accepted && atlas::Clock::now() < deadline) {
        std::this_thread::yield();
    }
    runner.Stop();

    // 🔴 A refused job runs NOTHING — not the work, not the completion. The alternative (a
    // completion invoked with a failure) was rejected because it would hand the caller two ways to
    // learn the same thing; what must never happen is the third option, disappearing in silence,
    // and that is what these two equalities pin down.
    EXPECT_EQ(ran.load(), accepted);
    EXPECT_EQ(completed.load(), accepted);
}

// 🔴 The regression line for the ban on overloading one return value with two meanings: `false`
// used to mean "stopped", and folding "overloaded" into it would leave the caller unable to tell a
// shutdown from a busy server (§10.6 is where the same overlap was taken apart before).
TEST_F(DbRuntimeTest, ARefusedSubmitIsDistinctFromAStoppedOne) {
    atlas::ConnectionPool pool(config_, 1);
    atlas::DbRunner runner(pool, 1, atlas::Seconds{2});

    // Never started.
    EXPECT_EQ(runner.Submit(Ctx{}, [](Ctx&, atlas::Connection*) {}), atlas::SubmitResult::Stopped);
    EXPECT_EQ(runner.RejectedCount(), 0U);

    runner.Start();
    std::atomic<bool> release{false};
    std::size_t rejected = 0;
    for (std::size_t index = 0; index < runner.QueueCapacity() + 8; ++index) {
        const atlas::SubmitResult result = runner.Submit(Ctx{}, [&](Ctx&, atlas::Connection*) {
            while (!release.load()) {
                std::this_thread::yield();
            }
        });
        if (result == atlas::SubmitResult::Rejected) {
            ++rejected;
        }
    }
    EXPECT_GT(rejected, 0U);
    EXPECT_EQ(runner.RejectedCount(), static_cast<UInt64>(rejected));

    release.store(true);
    runner.Stop();

    // Stopped after a drain, and the rejection count did not move: the two answers stay separate.
    EXPECT_EQ(runner.Submit(Ctx{}, [](Ctx&, atlas::Connection*) {}), atlas::SubmitResult::Stopped);
    EXPECT_EQ(runner.RejectedCount(), static_cast<UInt64>(rejected));
}

}  // namespace
