#pragma once
#include <string>

#include "atlas/core/types.h"

// architecture-design.md §5.4 — the `.env` half of the config boundary. 🔴 These keys never appear
// in server.ini: server.ini is committed, and a credential that reaches the repository cannot be
// taken back.
//
// 🔴 No value below is ever logged, formatted into an exception message, or streamed. The one
// diagnostic surface this type offers is DescribePresence(), which prints key names and set/empty
// and nothing else — that asymmetry is the whole point of the type.

namespace atlas {

struct SecretConfig {
    std::string db_host;
    UInt16 db_port{0};
    std::string db_name;
    std::string db_user;
    std::string db_password;
    std::string jwks_url;

    // architecture-design.md §10.2 — the cache. 🔴 Host and credentials only; what Redis is
    // allowed to be USED for is a design rule, not a setting, and there is deliberately no knob
    // here that could turn the forbidden roles on. An empty host means "no cache configured", and
    // the layer that wants one decides what to do about it — this one only transports.
    std::string redis_host;
    UInt16 redis_port{0};
    std::string redis_password;

    // `ATLAS_DB_TLS_NO_VERIFY` — not a secret, but it belongs to the same per-deployment axis as
    // the DB host, and server.ini cannot carry it because that file is baked into the image (§5.4).
    // 🔴 Defaults to false = verify. Only the literal string "1" turns it on, so a typo relaxes
    // nothing. What it costs is written on DbConnectionConfig::tls_no_verify.
    bool db_tls_no_verify{false};

    // Keys are read from the process environment (docker compose / `.env`). An unset key is an
    // empty value rather than a failure: this layer only transports the secrets, and the layer that
    // actually needs a DB connection is the one that gets to decide what is mandatory.
    [[nodiscard]] static SecretConfig FromEnvironment();

    // Key names + "<set>"/"<empty>". 🔴 Never a value.
    [[nodiscard]] std::string DescribePresence() const;

    void LogSummary() const;
};

}  // namespace atlas
