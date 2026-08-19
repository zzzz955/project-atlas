// =============================================================================
// server.ini 값을 타입 있는 ServerConfig 로 변환. 모든 실패는 기동 중단
// =============================================================================

#include "atlas/config/server_config.h"

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "atlas/core/error.h"

namespace atlas
{
namespace
{

constexpr std::string_view kServerSection = "server";
constexpr std::string_view kStackSection = "stack";
constexpr std::string_view kLogSection = "log";

std::string_view RequireIniValue( const IniDocument& document, std::string_view section,
                                  std::string_view key )
{
    const std::optional< std::string_view > value = document.Get( section, key );
    ATLAS_CHECK( value.has_value(), "server.ini: missing required key [{}] {}", section, key );
    return *value;
}

// [CS 4.1] 폭은 호출자가 명시한 선택이라 안 맞는 값은 조용한 wrap 이 아니라 변환 실패
template < class Unsigned >
Unsigned RequireIniUnsigned( const IniDocument& document, std::string_view section,
                             std::string_view key )
{
    const std::string_view text = RequireIniValue( document, section, key );
    Unsigned value{};
    const std::from_chars_result result =
        std::from_chars( text.data(), text.data() + text.size(), value );
    ATLAS_CHECK( result.ec == std::errc{} && result.ptr == text.data() + text.size(),
                 "server.ini: [{}] {} = '{}' does not fit the width this field is declared with",
                 section, key, text );
    return value;
}

ServerRole RequireIniRole( const IniDocument& document )
{
    const std::string_view text = RequireIniValue( document, kServerSection, "role" );
    if ( text == "fe" )
    {
        return ServerRole::Fe;
    }
    if ( text == "game" )
    {
        return ServerRole::Game;
    }
    if ( text == "world" )
    {
        return ServerRole::World;
    }
    if ( text == "interworld" )
    {
        return ServerRole::InterWorld;
    }
    ATLAS_THROW( Exception, "server.ini: unknown [server] role = '{}'", text );
}

LogLevel RequireIniLogLevel( const IniDocument& document )
{
    const std::string_view text = RequireIniValue( document, kLogSection, "level" );
    if ( text == "trace" )
    {
        return LogLevel::Trace;
    }
    if ( text == "debug" )
    {
        return LogLevel::Debug;
    }
    if ( text == "info" )
    {
        return LogLevel::Info;
    }
    if ( text == "warn" )
    {
        return LogLevel::Warn;
    }
    if ( text == "error" )
    {
        return LogLevel::Error;
    }
    if ( text == "fatal" )
    {
        return LogLevel::Fatal;
    }
    if ( text == "off" )
    {
        return LogLevel::Off;
    }
    ATLAS_THROW( Exception, "server.ini: unknown [log] level = '{}'", text );
}

}  // namespace

std::string_view RoleName( ServerRole role ) noexcept
{
    switch ( role )
    {
        case ServerRole::Fe:
            return "fe";
        case ServerRole::Game:
            return "game";
        case ServerRole::World:
            return "world";
        case ServerRole::InterWorld:
            return "interworld";
    }
    return "unknown";
}

ServerConfig ServerConfig::FromIni( const IniDocument& document )
{
    ServerConfig config;

    config.role = RequireIniRole( document );
    config.server_id = RequireIniUnsigned< UInt32 >( document, kServerSection, "server_id" );
    config.world_id = RequireIniUnsigned< UInt32 >( document, kServerSection, "world_id" );
    config.server_group = RequireIniUnsigned< UInt32 >( document, kServerSection, "server_group" );
    config.listen_port = RequireIniUnsigned< UInt16 >( document, kServerSection, "listen_port" );
    config.io_workers = RequireIniUnsigned< UInt32 >( document, kServerSection, "io_workers" );

    config.stack.client = std::string( RequireIniValue( document, kStackSection, "client" ) );
    config.stack.server = std::string( RequireIniValue( document, kStackSection, "server" ) );
    config.stack.db = std::string( RequireIniValue( document, kStackSection, "db" ) );
    config.stack.cache = std::string( RequireIniValue( document, kStackSection, "cache" ) );

    config.log.level = RequireIniLogLevel( document );
    config.log.dir = std::string( RequireIniValue( document, kLogSection, "dir" ) );
    config.log.retention_days =
        RequireIniUnsigned< UInt32 >( document, kLogSection, "retention_days" );

    return config;
}

ServerConfig ServerConfig::LoadFile( std::string_view path )
{
    return FromIni( IniDocument::LoadFile( path ) );
}

}  // namespace atlas
