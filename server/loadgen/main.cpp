#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <future>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include "atlas/config/secret_config.h"
#include "atlas/core/ctx.h"
#include "atlas/core/error.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/db/prepared_statement.h"
#include "atlas/net/io_runner.h"
#include "atlas/redis/connection.h"
#include "game/character_cache.h"
#include "game/inventory.h"
#include "game/ranking.h"
#include "generated/db/character_items_row.h"
#include "generated/db/characters_row.h"
#include "loadgen/load_client.h"

// The load harness binary (architecture-design.md §16 — the first load measurement, and §9.1 for
// the model load_client.h reuses).
//
//     atlas_loadgen --connections 64 --duration 20 --warmup 3 --io-threads 8
//
// Two optional flags add the ramp-up mode of §16.1i, which is what shows the server CROSSING its
// capacity rather than sitting under it:
//
//     --ramp 8,16,32,64,128    stage connection counts, strictly increasing
//     --stage-seconds <n>      how long each stage runs; --warmup is then the PER-STAGE prefix
//     --no-delay 0|1           TCP_NODELAY on THIS PROCESS's sockets; absent = untouched
//
// 🔴 With --ramp absent the harness is the fixed-concurrency mode every §16.1 table was measured
// with, unchanged. Ramp-up is an added mode: changing the default would delete the path back to
// those numbers.
//
// Three optional flags add the visualisation axis of §16.1a and nothing else:
//
//     --tui                    an ANSI screen refreshed once a second, off a thread of its own
//     --sample-jsonl <path>    one JSON record per second, the input of tools/loadreport
//     --probe-file <path>      a file an external fsync-probe loop keeps current, transcribed
//                              into every sample (§16.1c-① is the regime discriminator)
//
// 🔴 With none of them the harness behaves exactly as it did for the §16.1 tables — no sink, no
// sampler thread, same stdout. That is what keeps a run taken today comparable with them.
//
// 🔴 IT SEEDS ITS OWN DATA AND DELETES IT AGAIN. The scenario is connect -> load character -> equip
// in a loop, and equip needs a character that owns two items. Seeding a dedicated block of
// character ids (one per connection) is what keeps the per-character lock of §10.5 out of the
// measurement: with every connection on its own character, what serialises is the server's DB
// thread pool rather than one row's mutex.
//
// 🔴 "ITS OWN DATA" INCLUDES THE CACHE COPIES AND THE RANKING ENTRIES, not only the rows. Deleting
// the rows alone leaves a character:{server}:{id} copy that the next run can read for up to the
// TTL, and a ranking:{server}:exp member for a character that no longer exists — the ranking one is
// unbounded, it accumulates one entry per seeded id per run. This scenario happened to hide the
// first (every equip issues its own DEL) and never hid the second (§16.1h).
//
// 🔴 The numbers this prints are only meaningful next to the conditions they were taken under —
// hardware, build configuration, whether the client shares a CPU with the server, worker counts and
// pool sizes. Those live with the measurement table in architecture-design.md §16.1; a table of
// bare figures cannot be read and should not be trusted.

namespace {

using atlas::DbValue;
using atlas::Float64;
using atlas::Int64;
using atlas::UInt16;
using atlas::UInt32;
using atlas::UInt64;
using atlas::UInt8;

// Deleting the seeded block by an id RANGE, never by `server_id` alone: the harness runs against
// the same server_id the deployed GAME uses, and a delete scoped only to the server would take
// rows it did not create.
constexpr std::string_view kDeleteItemsSql =
    "DELETE FROM `character_items` WHERE `server_id` = ? AND `character_id` >= ? AND "
    "`character_id` < ?";
constexpr std::string_view kDeleteCharactersSql =
    "DELETE FROM `characters` WHERE `server_id` = ? AND `character_id` >= ? AND "
    "`character_id` < ?";

// The account every seeded character hangs off. One account with N characters is the ownership
// shape §6 describes, and it costs nothing to keep the harness honest about it.
constexpr UInt64 kAccountUid = 990000;

UInt64 ParseUnsigned(std::string_view text) {
    UInt64 value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const std::from_chars_result parsed = std::from_chars(first, last, value);
    ATLAS_CHECK(parsed.ec == std::errc{} && parsed.ptr == last, "'{}' is not a whole number", text);
    return value;
}

// `--ramp 8,16,32,64` — the stage list of §16.1i.
std::vector<std::size_t> ParseStageList(std::string_view text) {
    std::vector<std::size_t> stages;
    std::size_t begin = 0;
    while (true) {
        const std::size_t comma = text.find(',', begin);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        stages.push_back(static_cast<std::size_t>(ParseUnsigned(text.substr(begin, end - begin))));
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
    return stages;
}

// DATETIME in this schema has no fractional part.
atlas::SysTime NowToWholeSecond() {
    return std::chrono::floor<std::chrono::seconds>(atlas::SysClock::now());
}

UInt64 FirstItemOf(UInt64 character_id) { return (character_id * 10U) + 1U; }
UInt64 SecondItemOf(UInt64 character_id) { return (character_id * 10U) + 2U; }

void DeleteRange(atlas::Connection& connection, UInt16 server_id, UInt64 first_character,
                 std::size_t count) {
    const std::array<DbValue, 3> range{DbValue{static_cast<UInt64>(server_id)},
                                       DbValue{first_character},
                                       DbValue{first_character + static_cast<UInt64>(count)}};
    connection.Prepare(kDeleteItemsSql).Execute(range);
    connection.Prepare(kDeleteCharactersSql).Execute(range);
}

// How long the connect loop is given before the purge is attempted anyway, and how long one purge
// command may take. The second is above the 500 ms per-command deadline atlas/redis applies, so a
// timeout here means the answer never came rather than that the deadline was tight.
constexpr atlas::Seconds kRedisReadyBudget{2};
constexpr atlas::Seconds kRedisCommandBudget{2};

// One command, run from the process lifetime thread. 🔴 Never from an I/O thread: the reply lands
// on the connection's strand and this blocks until it does.
bool ExecuteBlocking(atlas::RedisConnection& redis, const atlas::RedisCommand& command) {
    auto answered = std::make_shared<std::promise<bool>>();
    std::future<bool> settled = answered->get_future();
    redis.Execute(atlas::Ctx{}, command,
                  [answered](const atlas::RedisResult& result) { answered->set_value(result.ok); });

    // shared_ptr, not a promise on this frame: the wait below is
    // bounded, so this can return while the handler is still
    // outstanding, and a frame-local promise would then be a
    // dangling write. The analyzer stops following the handler's
    // reference where it is posted onto the strand and reads the
    // control block as leaked. Same shape, and same marker, as
    // RedisConnection::WaitUntilReady.
    // The marker below is the last line before the code: it covers
    // one line only, and a comment would consume it.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    return settled.wait_for(kRedisCommandBudget) == std::future_status::ready && settled.get();
}

// The Redis half of DeleteRange: the cache copies and the ranking members of the seeded block.
//
// 🔴 SCOPED TO THE SEEDED ID RANGE, exactly like the SQL above and for the same reason — the
// harness runs against the same server_id the deployed GAME uses, so anything scoped to the server
// takes what this run did not create. There is no key-pattern scan here and there is no FLUSHDB.
//
// 🔴 Two commands, not 2N: DEL and ZREM both take the whole list.
void PurgeRedisRange(atlas::RedisConnection& redis, UInt16 server_id, UInt64 first_character,
                     std::size_t count) {
    // 🔴 The cache is OPTIONAL (§10.2). A run without one has no residue to clean and must not be
    // failed, or degraded on purpose, for the absence.
    if (!redis.IsConfigured()) {
        return;
    }

    std::vector<std::string> cache_keys;
    cache_keys.reserve(count);
    std::vector<std::string> ranking_args;
    ranking_args.reserve(count + 1U);
    ranking_args.push_back(atlas_demo::ExpRankingKey(server_id));
    for (std::size_t index = 0; index < count; ++index) {
        const UInt64 character_id = first_character + static_cast<UInt64>(index);
        cache_keys.push_back(atlas_demo::CharacterCacheKey(
            server_id, static_cast<atlas::CharacterId>(character_id)));
        ranking_args.push_back(std::to_string(character_id));
    }

    const bool cache_ok =
        ExecuteBlocking(redis, atlas::RedisCommand{.verb = "DEL", .args = std::move(cache_keys)});
    const bool ranking_ok = ExecuteBlocking(
        redis, atlas::RedisCommand{.verb = "ZREM", .args = std::move(ranking_args)});
    if (!cache_ok || !ranking_ok) {
        // Printed rather than logged: this is the harness talking to its operator, and the run's
        // figures are unaffected either way. Loud because the leftovers outlive the process.
        std::printf(
            "WARNING - redis cleanup incomplete (cache=%s ranking=%s). Seeded cache keys "
            "or ranking entries may survive this run.\n",
            cache_ok ? "ok" : "FAILED", ranking_ok ? "ok" : "FAILED");
    }
}

void Seed(atlas::Connection& connection, UInt16 server_id, UInt64 first_character,
          std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        const UInt64 character_id = first_character + static_cast<UInt64>(index);
        const std::array<DbValue, 10> character{DbValue{static_cast<UInt64>(server_id)},
                                                DbValue{character_id},
                                                DbValue{kAccountUid},
                                                DbValue{"load-" + std::to_string(character_id)},
                                                DbValue{Int64{0}},
                                                DbValue{Int64{0}},
                                                DbValue{UInt64{1}},
                                                DbValue{UInt64{0}},
                                                DbValue{NowToWholeSecond()},
                                                DbValue{std::monostate{}}};
        connection.Prepare(atlas::generated::kCharactersInsertSql).Execute(character);

        // 🔴 The first item starts IN the driven slot. Every request of the run therefore unequips
        // a real occupant and all three writes of §10.5 execute; starting from an empty slot would
        // leave write 1 idle and the harness would be timing a cheaper transaction than the server
        // actually runs.
        const std::array<DbValue, 6> worn{
            DbValue{static_cast<UInt64>(server_id)}, DbValue{character_id},
            DbValue{FirstItemOf(character_id)},
            // 🔴 1001 / 1002 are `Rusty Sword` / `Iron Sword`, slot 1, out of
            // shared/datas/item.csv. The equip path refuses an item_id the static data does not
            // define and an item claimed for the wrong slot (§8.2 layer 3), so seeding arbitrary
            // ids here would have the harness measuring the refusal path instead of the three-write
            // transaction.
            DbValue{UInt64{1001}}, DbValue{UInt64{1}},
            DbValue{static_cast<UInt64>(static_cast<UInt8>(atlas_demo::EquipSlot::Weapon))}};
        connection.Prepare(atlas::generated::kCharacterItemsInsertSql).Execute(worn);

        const std::array<DbValue, 6> spare{
            DbValue{static_cast<UInt64>(server_id)},
            DbValue{character_id},
            DbValue{SecondItemOf(character_id)},
            DbValue{UInt64{1002}},
            DbValue{UInt64{1}},
            DbValue{static_cast<UInt64>(static_cast<UInt8>(atlas_demo::EquipSlot::None))}};
        connection.Prepare(atlas::generated::kCharacterItemsInsertSql).Execute(spare);
    }
}

void Report(const atlas_loadgen::LoadOptions& options, const atlas_loadgen::LoadStats& stats) {
    std::vector<UInt32> samples = stats.latencies_us;
    std::ranges::sort(samples);

    const Float64 window_seconds = static_cast<Float64>(stats.steady_window_ms) / 1000.0;
    const Float64 throughput =
        window_seconds > 0.0 ? static_cast<Float64>(samples.size()) / window_seconds : 0.0;
    const UInt64 total_us = std::accumulate(samples.begin(), samples.end(), UInt64{0},
                                            [](UInt64 sum, UInt32 sample) { return sum + sample; });
    const Float64 mean_us =
        samples.empty() ? 0.0
                        : static_cast<Float64>(total_us) / static_cast<Float64>(samples.size());

    std::printf("connections=%zu rate=%u duration_s=%u warmup_s=%u io_threads=%zu no_delay=%s\n",
                options.connections, options.rate_per_second, options.duration_seconds,
                options.warmup_seconds, options.io_threads,
                options.no_delay.has_value() ? (*options.no_delay ? "1" : "0") : "unset");
    if (stats.no_delay_failures > 0) {
        // 🔴 Loud: the cell's label says one thing and its sockets did another.
        std::printf(
            "TCP_NODELAY REQUEST FAILED on %zu connections — this cell is not what it says\n",
            stats.no_delay_failures);
    }
    std::printf("established=%zu peak_live=%zu connect_fail=%zu transport_fail=%zu load_fail=%zu\n",
                stats.connections_established, stats.peak_live_connections, stats.connect_failures,
                stats.transport_failures, stats.load_failures);
    std::printf("requests_sent=%zu ok=%zu unavailable=%zu refused=%zu load_rejected=%zu\n",
                stats.requests_sent, stats.responses_ok, stats.responses_unavailable,
                stats.responses_refused, stats.loads_rejected);
    std::printf("steady_window_ms=%u samples=%zu throughput_rps=%.1f\n", stats.steady_window_ms,
                samples.size(), throughput);
    std::printf("mean_us=%.0f p50_us=%u p90_us=%u p99_us=%u p999_us=%u max_us=%u\n", mean_us,
                atlas_loadgen::Percentile(samples, 0.50), atlas_loadgen::Percentile(samples, 0.90),
                atlas_loadgen::Percentile(samples, 0.99), atlas_loadgen::Percentile(samples, 0.999),
                atlas_loadgen::Percentile(samples, 1.0));
    // 🔴 Printed only for a ramp. A fixed-concurrency run's single stage IS the block above, and
    // printing it twice would invite reading the repetition as a second measurement.
    if (stats.stages.size() > 1) {
        std::printf(
            "\n--- per stage --- (the aggregate above spans every stage and is therefore an "
            "average, not a capacity)\n");
        for (std::size_t index = 0; index < stats.stages.size(); ++index) {
            const atlas_loadgen::StageStats& stage = stats.stages[index];
            std::vector<UInt32> stage_samples = stage.latencies_us;
            std::ranges::sort(stage_samples);
            const Float64 stage_seconds = static_cast<Float64>(stage.window_ms) / 1000.0;
            const Float64 stage_throughput =
                stage_seconds > 0.0 ? static_cast<Float64>(stage_samples.size()) / stage_seconds
                                    : 0.0;
            const std::size_t answered =
                stage.responses_ok + stage.responses_rejected + stage.responses_refused;
            // 🔴 The rejection RATE, with its denominator on the same line. A rejection count alone
            // cannot be read: 600 out of 700 and 600 out of 60000 are opposite statements.
            const Float64 reject_percent =
                answered > 0 ? (static_cast<Float64>(stage.responses_rejected) * 100.0) /
                                   static_cast<Float64>(answered)
                             : 0.0;
            std::vector<UInt32> ok_samples = stage.ok_latencies_us;
            std::ranges::sort(ok_samples);
            const Float64 ok_throughput =
                stage_seconds > 0.0 ? static_cast<Float64>(ok_samples.size()) / stage_seconds : 0.0;
            std::printf(
                "stage=%zu connections=%zu window_ms=%u samples=%zu throughput_rps=%.1f ok=%zu "
                "ok_rps=%.1f rejected=%zu reject_pct=%.2f refused=%zu p50_us=%u p90_us=%u "
                "p99_us=%u max_us=%u ok_p50_us=%u ok_p90_us=%u ok_p99_us=%u ok_max_us=%u\n",
                index, stage.connections, stage.window_ms, stage_samples.size(), stage_throughput,
                stage.responses_ok, ok_throughput, stage.responses_rejected, reject_percent,
                stage.responses_refused, atlas_loadgen::Percentile(stage_samples, 0.50),
                atlas_loadgen::Percentile(stage_samples, 0.90),
                atlas_loadgen::Percentile(stage_samples, 0.99),
                atlas_loadgen::Percentile(stage_samples, 1.0),
                atlas_loadgen::Percentile(ok_samples, 0.50),
                atlas_loadgen::Percentile(ok_samples, 0.90),
                atlas_loadgen::Percentile(ok_samples, 0.99),
                atlas_loadgen::Percentile(ok_samples, 1.0));
        }
    }
    if (stats.watchdog_fired) {
        // 🔴 Loud, because every figure above is then suspect: a connection that never came back
        // contributed no samples, so the percentiles describe only the requests that survived.
        std::printf(
            "WATCHDOG FIRED — at least one connection never completed. Treat the "
            "percentiles above as a lower bound.\n");
    }
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT — the standard fixes main's signature.
    try {
        // Console only, warnings and worse: the harness is a client and its own log file would just
        // be another writer competing for the disk during a measurement.
        atlas::LogConfig log_config;
        log_config.level = atlas::LogLevel::Warn;
        atlas::LogInit(log_config);

        atlas_loadgen::LoadOptions options;
        const auto argument_count = static_cast<std::size_t>(argc);
        for (std::size_t index = 1; index < argument_count; ++index) {
            const std::string_view key(argv[index]);
            // The only option without a value, so it is taken before the pairing rule below.
            if (key == "--tui") {
                options.tui = true;
                continue;
            }
            ATLAS_CHECK(index + 1 < argument_count, "option '{}' needs a value", key);
            const std::string_view value(argv[index + 1]);
            ++index;
            if (key == "--host") {
                options.host = std::string(value);
            } else if (key == "--port") {
                options.port = static_cast<UInt16>(ParseUnsigned(value));
            } else if (key == "--connections") {
                options.connections = static_cast<std::size_t>(ParseUnsigned(value));
            } else if (key == "--rate") {
                options.rate_per_second = static_cast<UInt32>(ParseUnsigned(value));
            } else if (key == "--duration") {
                options.duration_seconds = static_cast<UInt32>(ParseUnsigned(value));
            } else if (key == "--warmup") {
                options.warmup_seconds = static_cast<UInt32>(ParseUnsigned(value));
            } else if (key == "--io-threads") {
                options.io_threads = static_cast<std::size_t>(ParseUnsigned(value));
            } else if (key == "--server-id") {
                options.server_id = static_cast<UInt16>(ParseUnsigned(value));
            } else if (key == "--first-character") {
                options.first_character_id = ParseUnsigned(value);
            } else if (key == "--no-delay") {
                options.no_delay = ParseUnsigned(value) != 0;
            } else if (key == "--ramp") {
                options.ramp_stages = ParseStageList(value);
            } else if (key == "--stage-seconds") {
                options.stage_seconds = static_cast<UInt32>(ParseUnsigned(value));
            } else if (key == "--sample-jsonl") {
                options.sample_jsonl_path = std::string(value);
            } else if (key == "--probe-file") {
                options.probe_file_path = std::string(value);
            } else {
                ATLAS_THROW(atlas::Exception, "unknown option '{}'", key);
            }
        }
        if (!options.ramp_stages.empty()) {
            ATLAS_CHECK(options.stage_seconds > 0, "--ramp needs --stage-seconds");
            // 🔴 CLOSED LOOP ONLY, and the reason is the phase spread of §16.1a. An open-loop ramp
            // would have to re-pace every connection at each stage boundary, because the
            // per-connection interval is the aggregate rate divided by a connection count that the
            // ramp changes. Getting that wrong reintroduces the thundering herd that once had this
            // harness reporting its own burst as 53 ms of server latency, and a ramp exists to find
            // the capacity limit — which is what a closed loop answers.
            ATLAS_CHECK(options.rate_per_second == 0,
                        "--ramp is closed loop only; drop --rate or drop --ramp");
            ATLAS_CHECK(options.warmup_seconds < options.stage_seconds,
                        "--warmup is the per-stage discarded prefix in ramp mode, so it must be "
                        "shorter than --stage-seconds");
            ATLAS_CHECK(options.ramp_stages.front() > 0, "--ramp stages must be at least 1");
            for (std::size_t index = 1; index < options.ramp_stages.size(); ++index) {
                // Connections join at a stage boundary and never leave, so a stage that asked for
                // fewer than its predecessor could not be honoured — and silently honouring the
                // larger number would report a ramp the run did not apply.
                ATLAS_CHECK(options.ramp_stages[index] > options.ramp_stages[index - 1],
                            "--ramp must be strictly increasing");
            }
            options.connections = options.ramp_stages.back();
            options.duration_seconds =
                options.stage_seconds * static_cast<UInt32>(options.ramp_stages.size());
        }
        ATLAS_CHECK(options.connections > 0, "--connections must be at least 1");
        ATLAS_CHECK(options.warmup_seconds < options.duration_seconds,
                    "--warmup must be shorter than --duration");

        const atlas::SecretConfig secrets = atlas::SecretConfig::FromEnvironment();
        ATLAS_CHECK(
            !secrets.db_host.empty() && !secrets.db_name.empty() && !secrets.db_user.empty(),
            "the database secrets are incomplete; see server/.env.example");

        atlas::DbConnectionConfig db_config;
        db_config.host = secrets.db_host;
        db_config.port = secrets.db_port == 0 ? UInt16{3306} : secrets.db_port;
        db_config.database = secrets.db_name;
        db_config.user = secrets.db_user;
        db_config.password = secrets.db_password;
        db_config.tls_no_verify = secrets.db_tls_no_verify;
#if defined(ATLAS_MARIADB_PLUGIN_DIR)
        db_config.plugin_directory = ATLAS_MARIADB_PLUGIN_DIR;
#endif

        atlas::RedisConnectionConfig redis_config;
        redis_config.host = secrets.redis_host;
        redis_config.port = secrets.redis_port == 0 ? UInt16{6379} : secrets.redis_port;
        redis_config.password = secrets.redis_password;
        // 🔴 The same rewrite server/tests/game_cache_test.cpp does, for the same reason: .env
        // carries the compose SERVICE name and this harness is a client that always runs on the
        // host, where that name is nothing. Unlike ATLAS_DB_HOST nothing outside does it for Redis.
        // An unset host is left alone — that is the cache being off (§10.2), not a name to fix.
        if (redis_config.host == "redis") {
            redis_config.host = "127.0.0.1";
        }

        // 🔴 Stopped before the io runner, never after (§9): the run loop is an outstanding
        // operation, and a context whose work guard is released while it is still pending never
        // drains. Declaration order gives that for free — redis is destroyed first.
        atlas::IoRunner redis_io(std::size_t{1});
        atlas::RedisConnection redis(redis_io.Context(), redis_config);
        if (redis.IsConfigured()) {
            redis_io.Start();
            redis.Start();
            if (!redis.WaitUntilReady(kRedisReadyBudget)) {
                std::printf(
                    "WARNING - redis is configured at %s but did not answer; this run's "
                    "cache and ranking cleanup will not happen.\n",
                    redis_config.host.c_str());
            }
        }

        atlas::Connection seeder(db_config);
        // Delete first: a previous run that was killed leaves its block behind, and INSERT would
        // then fail on the primary key rather than the harness starting clean. 🔴 The cache and the
        // ranking need the same treatment for a stronger reason: the ids are reused, so a copy left
        // by the previous run describes THIS run's character with the previous run's contents.
        DeleteRange(seeder, options.server_id, options.first_character_id, options.connections);
        PurgeRedisRange(redis, options.server_id, options.first_character_id, options.connections);
        Seed(seeder, options.server_id, options.first_character_id, options.connections);
        std::printf(
            "seeded %zu characters (server_id=%u, character_id %llu..%llu)\n", options.connections,
            options.server_id, static_cast<unsigned long long>(options.first_character_id),
            static_cast<unsigned long long>(options.first_character_id + options.connections - 1));

        const atlas_loadgen::LoadStats stats = atlas_loadgen::RunLoad(options);
        Report(options, stats);

        DeleteRange(seeder, options.server_id, options.first_character_id, options.connections);
        PurgeRedisRange(redis, options.server_id, options.first_character_id, options.connections);
        std::printf("seeded rows removed\n");

        redis.Stop();
        redis_io.Stop();
        atlas::LogShutdown();
        return 0;
    } catch (const std::exception& failure) {
        std::printf("loadgen failed: %s\n", failure.what());
        atlas::LogShutdown();
        return 70;
    }
}
