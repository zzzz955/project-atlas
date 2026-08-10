#include "atlas/config/secret_config.h"

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "atlas/core/error.h"
#include "atlas/core/log.h"

namespace atlas {
namespace {

// cpp-style.md §5 allows platform branches. MSVC deprecates std::getenv (C4996) and offers
// _dupenv_s, which allocates and hands ownership over; POSIX has the plain lookup.
std::optional<std::string> ReadSecretEnv(const char* name) {
#if defined(_MSC_VER)
    char* raw = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, name) != 0 || raw == nullptr) {
        return std::nullopt;
    }
    std::string value(raw);
    std::free(raw);
    return value;
#else
    const char* raw = std::getenv(name);
    if (raw == nullptr) {
        return std::nullopt;
    }
    return std::string(raw);
#endif
}

std::string ReadSecretString(const char* name) {
    return ReadSecretEnv(name).value_or(std::string{});
}

void AppendSecretPresence(std::string& out, std::string_view name, bool present) {
    if (!out.empty()) {
        out += ", ";
    }
    out += name;
    out += present ? "=<set>" : "=<empty>";
}

}  // namespace

SecretConfig SecretConfig::FromEnvironment() {
    SecretConfig config;
    config.db_host = ReadSecretString("ATLAS_DB_HOST");
    config.db_name = ReadSecretString("ATLAS_DB_NAME");
    config.db_user = ReadSecretString("ATLAS_DB_USER");
    config.db_password = ReadSecretString("ATLAS_DB_PASSWORD");
    config.jwks_url = ReadSecretString("ATLAS_JWKS_URL");
    // 🔴 Exact "1" only. An opt-out of certificate verification must not be reachable by "true",
    // "yes" or a stray space — every spelling this does not accept keeps verification on.
    config.db_tls_no_verify = ReadSecretString("ATLAS_DB_TLS_NO_VERIFY") == "1";

    const std::string port = ReadSecretString("ATLAS_DB_PORT");
    if (!port.empty()) {
        UInt16 value{0};
        const std::from_chars_result result =
            std::from_chars(port.data(), port.data() + port.size(), value);
        // 🔴 The offending text is deliberately absent from this message. Everything read here is
        // treated as a secret, and an error path that echoes its input is exactly how a credential
        // ends up in a log file that outlives the incident.
        ATLAS_CHECK(result.ec == std::errc{} && result.ptr == port.data() + port.size(),
                    "ATLAS_DB_PORT is not a valid port number");
        config.db_port = value;
    }

    return config;
}

std::string SecretConfig::DescribePresence() const {
    std::string summary;
    AppendSecretPresence(summary, "ATLAS_DB_HOST", !db_host.empty());
    AppendSecretPresence(summary, "ATLAS_DB_PORT", db_port != 0);
    AppendSecretPresence(summary, "ATLAS_DB_NAME", !db_name.empty());
    AppendSecretPresence(summary, "ATLAS_DB_USER", !db_user.empty());
    AppendSecretPresence(summary, "ATLAS_DB_PASSWORD", !db_password.empty());
    AppendSecretPresence(summary, "ATLAS_JWKS_URL", !jwks_url.empty());
    return summary;
}

void SecretConfig::LogSummary() const {
    // 🔴 The only log statement in this file, and it emits exactly DescribePresence(). That is what
    // turns "no secret value ever reaches the log" from a comment into something a test can pin.
    ATLAS_LOG_INFO("secrets loaded: {}", DescribePresence());
}

}  // namespace atlas
