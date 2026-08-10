#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <string_view>

#include "atlas/config/ini_document.h"
#include "atlas/config/secret_config.h"
#include "atlas/config/server_config.h"
#include "atlas/core/error.h"
#include "atlas/core/log.h"
#include "atlas/core/types.h"

namespace {

// Injected by server/tests/CMakeLists.txt. 🔴 The committed server.ini is found by absolute path:
// a test that assumed a working directory would pass under ctest and fail under the debugger.
constexpr std::string_view kServerIniPath = ATLAS_SERVER_INI_PATH;

void SetTestEnv(const char* name, const char* value) {
#if defined(_MSC_VER)
    static_cast<void>(_putenv_s(name, value));
#else
    static_cast<void>(setenv(name, value, 1));
#endif
}

// Every required key present, so each failure test varies exactly one field.
std::string MakeIni(std::string_view role, std::string_view listen_port) {
    std::string text = "[server]\nrole = ";
    text += role;
    text += "\nserver_id = 1\nworld_id = 0\nserver_group = 0\nlisten_port = ";
    text += listen_port;
    text += "\nio_workers = 0\n\n[stack]\nclient = godot\nserver = cpp-asio\ndb = mysql\n";
    text += "cache = none\n\n[log]\nlevel = info\ndir = logs\nretention_days = 14\n";
    return text;
}

// architecture-design.md §5.4 — the committed file is the spec, so the test reads the real one
// rather than a copy that can drift away from it.
TEST(ConfigServerIni, CommittedFileRoundTrips) {
    const atlas::ServerConfig config = atlas::ServerConfig::LoadFile(kServerIniPath);

    EXPECT_EQ(config.role, atlas::ServerRole::Game);
    EXPECT_EQ(atlas::RoleName(config.role), "game");
    EXPECT_EQ(config.server_id, atlas::UInt32{1});
    EXPECT_EQ(config.world_id, atlas::UInt32{0});
    EXPECT_EQ(config.server_group, atlas::UInt32{0});
    EXPECT_EQ(config.listen_port, atlas::UInt16{7777});
    EXPECT_EQ(config.io_workers, atlas::UInt32{0});
    EXPECT_EQ(config.log.level, atlas::LogLevel::Info);
    EXPECT_EQ(config.log.dir, "logs");
    EXPECT_EQ(config.log.retention_days, atlas::UInt32{14});
}

// §14 — the stack axis is data that is read and stored. Nothing branches on it.
TEST(ConfigServerIni, StackAxisIsReadAsData) {
    const atlas::ServerConfig config = atlas::ServerConfig::LoadFile(kServerIniPath);

    EXPECT_EQ(config.stack.client, "godot");
    EXPECT_EQ(config.stack.server, "cpp-asio");
    EXPECT_EQ(config.stack.db, "mysql");
    EXPECT_EQ(config.stack.cache, "redis");
}

// §5.2 — every role the topology defines has to survive the round trip, or the "same binary,
// different ini" rule is only true for the one role that happens to be committed.
TEST(ConfigServerIni, EveryRoleParses) {
    EXPECT_EQ(atlas::ServerConfig::FromIni(atlas::IniDocument::Parse(MakeIni("fe", "7777"))).role,
              atlas::ServerRole::Fe);
    EXPECT_EQ(
        atlas::ServerConfig::FromIni(atlas::IniDocument::Parse(MakeIni("world", "7777"))).role,
        atlas::ServerRole::World);
    EXPECT_EQ(
        atlas::ServerConfig::FromIni(atlas::IniDocument::Parse(MakeIni("interworld", "7777"))).role,
        atlas::ServerRole::InterWorld);
}

TEST(ConfigIniDocument, ParsesSectionsCommentsAndBlankLines) {
    const atlas::IniDocument document = atlas::IniDocument::Parse(
        "; leading comment\n\n[server]\n# another comment\n  role  =  game   ; trailing\n");

    EXPECT_EQ(document.Get("server", "role").value_or("<missing>"), "game");
    EXPECT_FALSE(document.Get("server", "absent").has_value());
    EXPECT_FALSE(document.Get("absent", "role").has_value());
}

// 🔴 Malformed input fails; it is never skipped. The static_cast<void> is what keeps /W4 quiet
// about the [[nodiscard]] factories - EXPECT_THROW only cares that the statement throws.
TEST(ConfigIniDocument, RejectsMalformedDocuments) {
    EXPECT_THROW(static_cast<void>(atlas::IniDocument::Parse("[server]\nrole = game\nrole = fe\n")),
                 atlas::Exception);
    EXPECT_THROW(static_cast<void>(atlas::IniDocument::Parse("role = game\n")), atlas::Exception);
    EXPECT_THROW(static_cast<void>(atlas::IniDocument::Parse("[server\nrole = game\n")),
                 atlas::Exception);
    EXPECT_THROW(static_cast<void>(atlas::IniDocument::LoadFile("no_such_file_here.ini")),
                 atlas::Exception);
}

TEST(ConfigServerIni, RejectsAnUnknownRole) {
    EXPECT_THROW(static_cast<void>(atlas::ServerConfig::FromIni(
                     atlas::IniDocument::Parse(MakeIni("lobby", "1")))),
                 atlas::Exception);
}

// cpp-style.md §4.1 — listen_port is a UInt16, so a value outside that width is a failure and not
// a silent wrap to 4464.
TEST(ConfigServerIni, RejectsAListenPortOutsideUInt16) {
    EXPECT_THROW(static_cast<void>(atlas::ServerConfig::FromIni(
                     atlas::IniDocument::Parse(MakeIni("game", "70000")))),
                 atlas::Exception);
    EXPECT_THROW(static_cast<void>(atlas::ServerConfig::FromIni(
                     atlas::IniDocument::Parse(MakeIni("game", "-1")))),
                 atlas::Exception);
}

TEST(ConfigServerIni, RejectsAMissingRequiredKey) {
    EXPECT_THROW(static_cast<void>(atlas::ServerConfig::FromIni(
                     atlas::IniDocument::Parse("[server]\nrole = game\n"))),
                 atlas::Exception);
}

// 🔴 architecture-design.md §5.4 — the loader prints key names, never values. This is the control
// that keeps a credential out of the log files.
TEST(ConfigSecrets, NeverPutsASecretValueIntoADiagnosticString) {
    constexpr const char* kSentinel = "SENTINEL-PASSWORD-9f3a1c";

    SetTestEnv("ATLAS_DB_HOST", "db.internal");
    SetTestEnv("ATLAS_DB_PORT", "3306");
    SetTestEnv("ATLAS_DB_NAME", "atlas");
    SetTestEnv("ATLAS_DB_USER", "atlas_user");
    SetTestEnv("ATLAS_DB_PASSWORD", kSentinel);
    SetTestEnv("ATLAS_JWKS_URL", "https://auth.example/.well-known/jwks.json");

    const atlas::SecretConfig secrets = atlas::SecretConfig::FromEnvironment();

    // Positive control: without this, "the value never shows up" could just mean it was never read.
    EXPECT_EQ(secrets.db_password, kSentinel);
    EXPECT_EQ(secrets.db_port, atlas::UInt16{3306});

    // Every string this module can emit: the presence summary (which LogSummary logs verbatim) and
    // the one error path it has.
    std::string diagnostics = secrets.DescribePresence();
    SetTestEnv("ATLAS_DB_PORT", "not-a-port");
    try {
        static_cast<void>(atlas::SecretConfig::FromEnvironment());
        FAIL() << "a malformed ATLAS_DB_PORT must be rejected";
    } catch (const atlas::Exception& ex) {
        diagnostics += ex.what();
    }

    EXPECT_EQ(diagnostics.find(kSentinel), std::string::npos);
    // Negative control: the key names are expected to be there - that is the diagnostic value.
    EXPECT_NE(diagnostics.find("ATLAS_DB_PASSWORD"), std::string::npos);
    EXPECT_NE(diagnostics.find("ATLAS_DB_PORT"), std::string::npos);

    atlas::SetLogLevel(atlas::LogLevel::Info);
    const atlas::UInt64 info_before = atlas::LogCount(atlas::LogLevel::Info);
    secrets.LogSummary();
    EXPECT_EQ(atlas::LogCount(atlas::LogLevel::Info), info_before + 1);

    SetTestEnv("ATLAS_DB_PASSWORD", "");
    SetTestEnv("ATLAS_DB_PORT", "");
}

TEST(ConfigSecrets, MissingKeysAreEmptyRatherThanAFailure) {
    SetTestEnv("ATLAS_DB_PASSWORD", "");
    SetTestEnv("ATLAS_DB_PORT", "");

    const atlas::SecretConfig secrets = atlas::SecretConfig::FromEnvironment();
    const std::string summary = secrets.DescribePresence();

    EXPECT_TRUE(secrets.db_password.empty());
    EXPECT_EQ(secrets.db_port, atlas::UInt16{0});
    EXPECT_NE(summary.find("ATLAS_DB_PASSWORD=<empty>"), std::string::npos);
    EXPECT_NE(summary.find("ATLAS_DB_PORT=<empty>"), std::string::npos);
}

}  // namespace
