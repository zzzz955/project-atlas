#include <atomic>
#include <csignal>
#include <cstddef>
#include <exception>
#include <string>
#include <thread>

#include "atlas/config/secret_config.h"
#include "atlas/config/server_config.h"
#include "atlas/core/log.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/net/net_types.h"
#include "atlas/redis/connection.h"
#include "game/handlers.h"

// The GAME server binary (architecture-design.md §5.1 · §15.3).
//
// 🔴 Single image, role-branching entrypoint. `scripts/entrypoint.sh` maps ATLAS_ROLE -> binary and
// this slice makes `game` real; `fe` and `world` must keep failing loudly, because §15.3's whole
// point is that nothing is wrapped to look present when it is not.
//
// 🔴 The wiring is in game/handlers.cpp, not here. A main() that owned it would make the end-to-end
// test build a second, different server — see the note at the top of handlers.h.

namespace {

// 🔴 The only thing a graceful-stop signal handler does is store a flag. Crash diagnostics own only
// fatal signals; SIGINT/SIGTERM must reach the ordered shutdown in §9, where LogShutdown flushes on
// the main thread. Doing no logger work here is what keeps this handler async-signal-safe.
std::atomic<bool> g_stop_requested{false};  // NOLINT — a signal flag has to be at namespace scope.

extern "C" void OnStopSignal(int /*signal_number*/) { g_stop_requested.store(true); }

// Where server.ini is: argv[1], else the working directory — which is /app in the image, next to
// the ini the Dockerfile copies there. 🔴 One override, not two: an environment variable for this
// as well would put the same answer in two places, and §5.4's boundary is about which source owns
// which fact.
std::string IniPath(const char* argument) {
    return argument == nullptr ? std::string("server.ini") : std::string(argument);
}

// 🔴 architecture-design.md §10.3 — not read from server.ini, and the reason is honesty rather than
// preference: the typed ini view lives in atlas/config and this slice did not widen it, so putting
// keys in the committed ini that nothing parses would be decoration. They move into `[server]` the
// moment ServerConfig learns to read them.
constexpr std::size_t kDbPoolSize = 4;
constexpr std::size_t kDbThreads = 2;

}  // namespace

int main(int argc, char** argv) {  // NOLINT — the standard fixes main's signature.
    try {
        const atlas::ServerConfig config =
            atlas::ServerConfig::LoadFile(IniPath(argc > 1 ? argv[1] : nullptr));

        atlas::LogConfig log_config;
        log_config.directory = config.log.dir;
        log_config.basename = "atlas_game";
        log_config.level = config.log.level;
        log_config.retain_days = static_cast<atlas::UInt16>(config.log.retention_days);
        atlas::LogInit(log_config);

        // 🔴 §5.4 — ATLAS_ROLE picks the binary, `[server] role` is the runtime identity, and they
        // must agree. A GAME binary holding a WORLD ini is exactly the mismatch that boundary
        // exists to catch, and it has to be a start-up failure: a server that boots into an
        // identity nobody chose is worse than one that does not boot.
        if (config.role != atlas::ServerRole::Game) {
            ATLAS_LOG_FATAL("this is the GAME binary but server.ini says role={}",
                            atlas::RoleName(config.role));
            atlas::LogShutdown();
            return 64;
        }

        const atlas::SecretConfig secrets = atlas::SecretConfig::FromEnvironment();
        // 🔴 Key names and set/empty only — never a value (§5.4).
        secrets.LogSummary();
        if (secrets.db_host.empty() || secrets.db_name.empty() || secrets.db_user.empty()) {
            ATLAS_LOG_FATAL("the database secrets are incomplete; see server/.env.example");
            atlas::LogShutdown();
            return 78;
        }

        atlas::DbConnectionConfig db_config;
        db_config.host = secrets.db_host;
        db_config.port = secrets.db_port == 0 ? atlas::UInt16{3306} : secrets.db_port;
        db_config.database = secrets.db_name;
        db_config.user = secrets.db_user;
        db_config.password = secrets.db_password;
        db_config.tls_no_verify = secrets.db_tls_no_verify;
        if (db_config.tls_no_verify) {
            // 🔴 A relaxed TLS posture that leaves no trace in the log is indistinguishable from a
            // mistake, so it announces itself once, at WARN, every single boot. See
            // DbConnectionConfig::tls_no_verify for what is actually given up.
            ATLAS_LOG_WARN(
                "ATLAS_DB_TLS_NO_VERIFY=1 — the database connection is encrypted but the server "
                "certificate is NOT verified. Local/compose only; never production.");
        }

        // 🔴 architecture-design.md §10.2 — the cache is OPTIONAL and its absence is a warning,
        // not a failure. An unset host means every load goes to the database, which is also what
        // happens when Redis is reachable but a lookup fails: one degraded path, not two.
        atlas::RedisConnectionConfig redis_config;
        redis_config.host = secrets.redis_host;
        redis_config.port = secrets.redis_port == 0 ? atlas::UInt16{6379} : secrets.redis_port;
        redis_config.password = secrets.redis_password;
        if (redis_config.host.empty()) {
            ATLAS_LOG_WARN(
                "ATLAS_REDIS_HOST is unset — the character read cache and the exp ranking are off "
                "and every character load goes to the database. See server/.env.example.");
        }

        atlas_demo::GameServer::Options options;
        options.endpoint = atlas::Endpoint(atlas::Tcp::v4(), config.listen_port);
        options.server_id = static_cast<atlas::UInt16>(config.server_id);
        options.io_workers = static_cast<std::size_t>(config.io_workers);
        options.db_pool_size = kDbPoolSize;
        options.db_threads = kDbThreads;

        atlas_demo::GameServer server(options, db_config, redis_config);

        // Graceful stop is separate from crash.cpp's fatal-signal path: it drains instead of
        // producing a dump.
        std::signal(SIGINT, &OnStopSignal);
        std::signal(SIGTERM, &OnStopSignal);

        server.Start();

        // 🔴 Polled rather than an asio signal_set on the server's own io_context: the handler
        // would run on a worker thread, and Stop() joins those workers — a thread cannot join
        // itself.
        while (!g_stop_requested.load()) {
            std::this_thread::sleep_for(atlas::Millis{200});
        }

        ATLAS_LOG_INFO("stop requested — draining");
        server.Stop();
        ATLAS_LOG_INFO("GAME stopped");
        atlas::LogShutdown();
        return 0;
    } catch (const std::exception& failure) {
        // Start-up failures land here: a port already taken, a database that refuses the
        // credentials, a malformed ini. All of them must be loud and fatal rather than degraded.
        ATLAS_LOG_FATAL("GAME failed to start: {}", failure.what());
        atlas::LogShutdown();
        return 70;
    }
}
