#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "atlas/core/error.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"

// architecture-design.md §10 — the ORM runtime. This header is the umbrella of the layer: the
// shared vocabulary (value type, row, error type, connection settings) plus the connection itself.
// The generated slice (server/generated/db, §10.1) produced row structs, column metadata and fixed
// prepared-statement text; this slice produces the part that executes.
//
// 🔴 core-purity (§15.4) forbids server/atlas/** from including anything under server/generated/**,
// and the generated row headers carry demo-game vocabulary. So this layer is deliberately GENERIC:
// it binds and reads columns as values and knows no table. The row-aware code that maps a generated
// struct onto these primitives lives in the game layer, above the core.
//
// 🔴 <mysql.h> is included by the .cpp files of this library and by no header. The driver handle is
// named here by its own C API struct tag (`typedef struct st_mysql MYSQL;`) rather than laundered
// through a void*, so a driver that ever renamed it would break the build loudly instead of
// silently reinterpreting a pointer. The mechanical enforcement of the hiding is the PRIVATE link
// to unofficial::libmariadb in CMakeLists.txt — the same shape core/log.h uses to keep spdlog out
// of every translation unit (§11.1).
// NOLINTNEXTLINE(readability-identifier-naming) — the tag belongs to libmariadb's C API, not to us;
// renaming it to satisfy §3 would stop it from being a forward declaration of the driver's struct.
struct st_mysql;

namespace atlas {

class PreparedStatement;

// Driver failures. Distinct from atlas::Exception only so a caller can catch the database apart
// from everything else; there is no hierarchy below it (cpp-style.md §2 — no speculative layers).
//
// architecture-design.md §11.2(a): an expected failure is not an exception. Pool exhaustion returns
// std::nullopt and a missing row is an empty result — those paths do not throw. What throws here is
// a statement the server got wrong or a database that is not answering, which is the unrecoverable
// half of the policy.
class DbException : public Exception {
public:
    using Exception::Exception;
};

// One generic column value.
//
// 🔴 The alternatives are the widest fixed-width forms, not one per SQL type: every signed integer
// column arrives as Int64 and every unsigned one as UInt64. Narrowing back to the column's declared
// width is the row mapper's job, and it has the generated ColumnMeta to do it with. std::monostate
// is SQL NULL.
using DbValue = std::variant<std::monostate, Int64, UInt64, Float64, std::string, SysTime>;

// One result row, in the column order the statement selected.
using DbRow = std::vector<DbValue>;

// 🔴 Every field here comes from `.env` via atlas::SecretConfig (§5.4), never from server.ini, and
// no field is ever logged or formatted into an exception message. This struct exists so that
// atlas/db does not have to link the config layer to be handed its credentials.
struct DbConnectionConfig {
    std::string host{};
    UInt16 port{3306};
    std::string database{};
    std::string user{};
    std::string password{};

    // Where the driver looks for its authentication plugins. Empty means "whatever the driver was
    // built with", which is the right answer for a normally installed libmariadb.
    //
    // 🔴 Not a secret and not speculative configurability — it is the fix for a failure that
    // reports itself as something else. MySQL 8.4 authenticates with caching_sha2_password, and in
    // this driver that lives in a separate plugin DLL rather than in the client library. A client
    // that cannot find its plugin directory (vcpkg bakes in the absolute path of its own build
    // tree, which does not survive installation) fails the handshake, and the SERVER answers plain
    // "1045 access denied" — so the symptom points at the credentials and nothing anywhere
    // mentions a plugin. Measured 2026-08-10 against the compose `mysql` service.
    std::string plugin_directory{};

    // Encrypt the connection but 🔴 do NOT verify the server's certificate. `false` — the default
    // everywhere — leaves the driver's own posture alone, which since MariaDB Connector/C 3.4 is
    // TLS **plus** full certificate and hostname verification. This flag can only ever relax that.
    //
    // 🔴 What it gives up, stated plainly: any certificate is accepted, so the channel is protected
    // against passive sniffing but NOT against an active man-in-the-middle. Production must not run
    // with this on. It is not a convenience knob and it is not a default.
    //
    // Why it exists: the compose stack (§15.3) talks to the stock `mysql:8.4` image, which
    // generates its own self-signed CA on first init. Connector/C 3.4.8 completes the TLS handshake
    // and then rejects that chain — `2026 / TLS/SSL error: self-signed certificate in certificate
    // chain` — so the GAME container died at start-up before it ever listened. Measured 2026-08-10
    // against the compose `mysql` service; the alternative (feed the client MySQL's generated
    // ca.pem) fails the hostname check on top of needing a file out of the mysql data volume.
    // §15.2.
    //
    // Carried from the environment as `ATLAS_DB_TLS_NO_VERIFY` (§5.4) and logged as a warning at
    // start-up, because a downgrade nobody can see in the logs is the thing this comment forbids.
    bool tls_no_verify{false};
};

// A single driver connection plus its prepared-statement cache.
//
// 🔴 NOT thread-safe, and deliberately so. A connection is leased out of ConnectionPool to exactly
// one thread for the duration of the lease, which is what makes the whole layer lock-free below the
// pool itself. Sharing one across threads corrupts the driver's per-connection state.
class Connection {
public:
    // Connects eagerly; throws DbException when the server refuses.
    explicit Connection(const DbConnectionConfig& config);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    // Returns the cached statement for `sql`, preparing it on first use only.
    //
    // 🔴 The cache exists to remove the parse/plan round trip — that is its purpose. Blocking SQL
    // injection is a CONSEQUENCE of never assembling statement text at run time, not the reason
    // the cache is here. Stating the order matters: someone who believes the cache is a security
    // device eventually "optimises" it away for a query they think is safe.
    PreparedStatement& Prepare(std::string_view sql);

    // How many statements this connection has actually prepared. The regression line under
    // "a cached statement is not re-prepared" is an assertion on this counter.
    [[nodiscard]] UInt64 PrepareCount() const noexcept { return prepare_count_; }

private:
    // Transaction control is private because the RAII scope is the only correct way to use it
    // (architecture-design.md §10: a scope that unwinds without a commit must roll back).
    friend class Transaction;

    void Begin();
    void Commit();
    void Rollback() noexcept;
    void RestoreAutoCommit() noexcept;

    st_mysql* handle_{nullptr};
    std::unordered_map<std::string, std::unique_ptr<PreparedStatement>> statements_;
    UInt64 prepare_count_{0};
};

}  // namespace atlas
