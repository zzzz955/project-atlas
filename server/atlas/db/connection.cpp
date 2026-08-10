#include "atlas/db/connection.h"

#include <mysql.h>

#include <memory>
#include <string>
#include <utility>

#include "atlas/core/log.h"
#include "atlas/db/prepared_statement.h"

namespace atlas {

Connection::Connection(const DbConnectionConfig& config) {
    handle_ = mysql_init(nullptr);
    if (handle_ == nullptr) {
        ATLAS_THROW(DbException, "mysql_init failed");
    }

    // The schema is utf8mb4 (server/generated/db/schema.sql). Negotiating it here rather than
    // relying on the server default keeps a name column round-tripping byte for byte.
    mysql_options(handle_, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    // See DbConnectionConfig::plugin_directory — without this the caching_sha2_password plugin can
    // go missing and the handshake fails as "access denied" instead of "plugin not found".
    if (!config.plugin_directory.empty()) {
        mysql_options(handle_, MYSQL_PLUGIN_DIR, config.plugin_directory.c_str());
    }

    // See DbConnectionConfig::tls_no_verify. Nothing is touched on the default path: the driver
    // already negotiates TLS and verifies the peer, and that is the posture to keep.
    if (config.tls_no_verify) {
        my_bool enforce = 1;
        my_bool verify = 0;
        // 🔴 Both flags, and ENFORCE is not optional here. Measured 2026-08-10: clearing the verify
        // flag ALONE makes Connector/C 3.4.8 abandon TLS altogether and connect in cleartext
        // (mysql_get_ssl_cipher() returns nullptr). "Do not check the certificate" quietly becoming
        // "do not encrypt" is exactly the silent downgrade this option must never cause, so ENFORCE
        // pins the channel encrypted and only the identity check is surrendered.
        mysql_options(handle_, MYSQL_OPT_SSL_ENFORCE, &enforce);
        mysql_options(handle_, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify);
    }

    if (mysql_real_connect(handle_, config.host.c_str(), config.user.c_str(),
                           config.password.c_str(), config.database.c_str(), config.port, nullptr,
                           0) == nullptr) {
        // 🔴 The driver's message is deliberately NOT in here. A failed handshake reports back
        // "Access denied for user 'x'@'host'", and host/user come straight from .env — §5.4 says
        // secret values never reach a log line or an exception message, and an exception message
        // is the one that gets pasted into a bug report. The numeric code says everything the
        // operator needs (1045 = access denied, 2003 = cannot reach the server) and leaks nothing.
        const UInt32 code = mysql_errno(handle_);
        mysql_close(handle_);
        handle_ = nullptr;
        ATLAS_THROW(DbException, "db connect failed: driver error {}", code);
    }
}

Connection::~Connection() {
    // 🔴 Statements first: a MYSQL_STMT belongs to the MYSQL it was prepared on, and closing the
    // connection out from under one is a use-after-free inside the driver. Member destruction order
    // would happen to get this right, but "happens to" is not a guarantee worth resting on.
    statements_.clear();
    if (handle_ != nullptr) {
        mysql_close(handle_);
        handle_ = nullptr;
    }
}

PreparedStatement& Connection::Prepare(std::string_view sql) {
    std::string key(sql);
    const auto found = statements_.find(key);
    if (found != statements_.end()) {
        return *found->second;
    }

    auto statement = std::make_unique<PreparedStatement>(handle_, key);
    PreparedStatement& created = *statement;
    statements_.emplace(std::move(key), std::move(statement));
    ++prepare_count_;
    return created;
}

void Connection::Begin() {
    // 🔴 mysql_autocommit / mysql_commit / mysql_rollback, never a "START TRANSACTION" string sent
    // through mysql_query. The dedicated calls carry no text at all, so the rule "this layer never
    // builds SQL" holds for transaction control too and not merely for data statements.
    if (mysql_autocommit(handle_, 0) != 0) {
        ATLAS_THROW(DbException, "db begin failed: {}", mysql_error(handle_));
    }
}

void Connection::Commit() {
    const bool committed = mysql_commit(handle_) == 0;
    const std::string reason = committed ? std::string{} : std::string(mysql_error(handle_));
    // Restored on both paths: a connection that went back to the pool with autocommit still off
    // would silently open a transaction around the next unrelated statement.
    RestoreAutoCommit();
    if (!committed) {
        ATLAS_THROW(DbException, "db commit failed: {}", reason);
    }
}

void Connection::Rollback() noexcept {
    // noexcept because the caller is a destructor unwinding an exception (architecture-design.md
    // §10 — the transaction scope rolls back when it unwinds). Throwing from here during that
    // unwind terminates the process, so a failed rollback is logged and nothing more.
    if (mysql_rollback(handle_) != 0) {
        // The same shape Guarded::Fail uses (core/error.h): the reporting path of a noexcept
        // function has to be total too, because std::format runs at the call site and can throw.
        try {
            ATLAS_LOG_ERROR("db rollback failed: {}", mysql_error(handle_));
        } catch (...) {  // NOLINT — nowhere left to report to.
        }
    }
    RestoreAutoCommit();
}

void Connection::RestoreAutoCommit() noexcept {
    if (mysql_autocommit(handle_, 1) != 0) {
        try {
            ATLAS_LOG_ERROR("db autocommit restore failed: {}", mysql_error(handle_));
        } catch (...) {  // NOLINT — see Rollback.
        }
    }
}

}  // namespace atlas
