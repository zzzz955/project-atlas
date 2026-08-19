#pragma once

// =============================================================================
// [AD 5.4] server.ini 의 타입 있는 뷰
// =============================================================================

#include <string>
#include <string_view>

#include "atlas/config/ini_document.h"
#include "atlas/core/log.h"
#include "atlas/core/types.h"

namespace atlas
{

// [AD 5.2] WORLD 와 InterWorld 는 같은 바이너리라 ini 만이 둘을 가름
// string 이면 오타가 라우팅 결정까지 흘러감. 모르는 role 은 기동 실패
enum class ServerRole : UInt8
{
    Fe,
    Game,
    World,
    InterWorld
};

[[nodiscard]] std::string_view RoleName( ServerRole role ) noexcept;

// [AD 14] 배포/스택 축. 읽어 담기만 하고 분기하는 코드는 일부러 없음
struct StackConfig
{
    std::string client;
    std::string server;
    std::string db;
    std::string cache;
};

// LogConfig 가 아닌 이유는 atlas::LogConfig 가 로거 자신의 런타임 구조체라서
struct LogSettings
{
    LogLevel level{ LogLevel::Info };
    std::string dir;
    UInt32 retention_days{ 0 };
};

struct ServerConfig
{
    ServerRole role{ ServerRole::Game };
    UInt32 server_id{ 0 };
    UInt32 world_id{ 0 };
    UInt32 server_group{ 0 };
    UInt16 listen_port{ 0 };
    // [AD 9] 0 = std::thread::hardware_concurrency
    UInt32 io_workers{ 0 };
    StackConfig stack;
    LogSettings log;

    [[nodiscard]] static ServerConfig FromIni( const IniDocument& document );
    [[nodiscard]] static ServerConfig LoadFile( std::string_view path );
};

}  // namespace atlas
