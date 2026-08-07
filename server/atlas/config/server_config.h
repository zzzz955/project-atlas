#pragma once
#include <string>
#include <string_view>

#include "atlas/config/ini_document.h"
#include "atlas/core/log.h"
#include "atlas/core/types.h"

// architecture-design.md §5.4 — the typed view of `server/server.ini`.

namespace atlas {

// 🔴 §5.2 — WORLD and InterWorld are the same binary and the ini is the only thing that tells them
// apart. An `enum class` rather than a string: an unknown role has to be a start-up failure, and a
// string would let a typo travel all the way into a routing decision.
enum class ServerRole : UInt8 { Fe, Game, World, InterWorld };

[[nodiscard]] std::string_view RoleName(ServerRole role) noexcept;

// §14 — the deploy/stack axis is described as data and only read. 🔴 There is deliberately no code
// that branches on these values; the axis is promoted to a stack_generator only when a second
// combination actually exists and runs in the CI gate.
struct StackConfig {
    std::string client;
    std::string server;
    std::string db;
    std::string cache;
};

// Named LogSettings, not LogConfig: atlas::LogConfig (core/log.h) is the logger's own runtime
// struct, and two types with the same name in one namespace is not a choice available here.
struct LogSettings {
    LogLevel level{LogLevel::Info};
    std::string dir;
    UInt32 retention_days{0};
};

struct ServerConfig {
    ServerRole role{ServerRole::Game};
    UInt32 server_id{0};
    UInt32 world_id{0};
    UInt32 server_group{0};
    UInt16 listen_port{0};
    // 0 = std::thread::hardware_concurrency (§9).
    UInt32 io_workers{0};
    StackConfig stack;
    LogSettings log;

    // A missing required key, a value that does not convert, or an unknown role is a failure -
    // never a default that boots the process into an identity nobody chose.
    [[nodiscard]] static ServerConfig FromIni(const IniDocument& document);
    [[nodiscard]] static ServerConfig LoadFile(std::string_view path);
};

}  // namespace atlas
