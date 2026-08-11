#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <numeric>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include "atlas/config/secret_config.h"
#include "atlas/core/error.h"
#include "atlas/core/log.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/db/prepared_statement.h"
#include "game/inventory.h"
#include "generated/db/character_items_row.h"
#include "generated/db/characters_row.h"
#include "loadgen/load_client.h"

// The load harness binary (architecture-design.md §16 — the first load measurement, and §9.1 for
// the model load_client.h reuses).
//
//     atlas_loadgen --connections 64 --duration 20 --warmup 3 --io-threads 8
//
// Two optional flags add the visualisation axis of §16.1a and nothing else:
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

    std::printf("connections=%zu rate=%u duration_s=%u warmup_s=%u io_threads=%zu\n",
                options.connections, options.rate_per_second, options.duration_seconds,
                options.warmup_seconds, options.io_threads);
    std::printf("established=%zu peak_live=%zu connect_fail=%zu transport_fail=%zu load_fail=%zu\n",
                stats.connections_established, stats.peak_live_connections, stats.connect_failures,
                stats.transport_failures, stats.load_failures);
    std::printf("requests_sent=%zu ok=%zu unavailable=%zu refused=%zu\n", stats.requests_sent,
                stats.responses_ok, stats.responses_unavailable, stats.responses_refused);
    std::printf("steady_window_ms=%u samples=%zu throughput_rps=%.1f\n", stats.steady_window_ms,
                samples.size(), throughput);
    std::printf("mean_us=%.0f p50_us=%u p90_us=%u p99_us=%u p999_us=%u max_us=%u\n", mean_us,
                atlas_loadgen::Percentile(samples, 0.50), atlas_loadgen::Percentile(samples, 0.90),
                atlas_loadgen::Percentile(samples, 0.99), atlas_loadgen::Percentile(samples, 0.999),
                atlas_loadgen::Percentile(samples, 1.0));
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
            } else if (key == "--sample-jsonl") {
                options.sample_jsonl_path = std::string(value);
            } else if (key == "--probe-file") {
                options.probe_file_path = std::string(value);
            } else {
                ATLAS_THROW(atlas::Exception, "unknown option '{}'", key);
            }
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

        atlas::Connection seeder(db_config);
        // Delete first: a previous run that was killed leaves its block behind, and INSERT would
        // then fail on the primary key rather than the harness starting clean.
        DeleteRange(seeder, options.server_id, options.first_character_id, options.connections);
        Seed(seeder, options.server_id, options.first_character_id, options.connections);
        std::printf(
            "seeded %zu characters (server_id=%u, character_id %llu..%llu)\n", options.connections,
            options.server_id, static_cast<unsigned long long>(options.first_character_id),
            static_cast<unsigned long long>(options.first_character_id + options.connections - 1));

        const atlas_loadgen::LoadStats stats = atlas_loadgen::RunLoad(options);
        Report(options, stats);

        DeleteRange(seeder, options.server_id, options.first_character_id, options.connections);
        std::printf("seeded rows removed\n");

        atlas::LogShutdown();
        return 0;
    } catch (const std::exception& failure) {
        std::printf("loadgen failed: %s\n", failure.what());
        atlas::LogShutdown();
        return 70;
    }
}
