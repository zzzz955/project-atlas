// =============================================================================
// [AD 5.4] server.ini 파싱과 .env 비밀값 분리 규약 검사
// =============================================================================

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

namespace
{

// CMakeLists.txt 가 주입. 작업 디렉터리를 가정하면 디버거에서 실패
constexpr std::string_view kServerIniPath = ATLAS_SERVER_INI_PATH;

void SetTestEnv( const char* name, const char* value )
{
#if defined( _MSC_VER )
    static_cast< void >( _putenv_s( name, value ) );
#else
    static_cast< void >( setenv( name, value, 1 ) );
#endif
}

std::string MakeIni( std::string_view role, std::string_view listen_port )
{
    std::string text = "[server]\nrole = ";
    text += role;
    text += "\nserver_id = 1\nworld_id = 0\nserver_group = 0\nlisten_port = ";
    text += listen_port;
    text += "\nio_workers = 0\n\n[stack]\nclient = godot\nserver = cpp-asio\ndb = mysql\n";
    text += "cache = none\n\n[log]\nlevel = info\ndir = logs\nretention_days = 14\n";
    return text;
}

// [AD 5.4] 커밋된 파일이 곧 명세. 사본은 드리프트하므로 실물을 읽음
TEST( ConfigServerIni, CommittedFileRoundTrips )
{
    const atlas::ServerConfig config = atlas::ServerConfig::LoadFile( kServerIniPath );

    EXPECT_EQ( config.role, atlas::ServerRole::Game );
    EXPECT_EQ( atlas::RoleName( config.role ), "game" );
    EXPECT_EQ( config.server_id, atlas::UInt32{ 1 } );
    EXPECT_EQ( config.world_id, atlas::UInt32{ 0 } );
    EXPECT_EQ( config.server_group, atlas::UInt32{ 0 } );
    EXPECT_EQ( config.listen_port, atlas::UInt16{ 7777 } );
    EXPECT_EQ( config.io_workers, atlas::UInt32{ 0 } );
    EXPECT_EQ( config.log.level, atlas::LogLevel::Info );
    EXPECT_EQ( config.log.dir, "logs" );
    EXPECT_EQ( config.log.retention_days, atlas::UInt32{ 14 } );
}

// [AD 14] stack 축은 읽어서 보관만 하는 데이터. 여기에 분기가 걸리지 않음
TEST( ConfigServerIni, StackAxisIsReadAsData )
{
    const atlas::ServerConfig config = atlas::ServerConfig::LoadFile( kServerIniPath );

    EXPECT_EQ( config.stack.client, "godot" );
    EXPECT_EQ( config.stack.server, "cpp-asio" );
    EXPECT_EQ( config.stack.db, "mysql" );
    EXPECT_EQ( config.stack.cache, "redis" );
}

// [AD 5.2] 모든 role 이 왕복해야 "같은 바이너리, 다른 ini" 가 성립
TEST( ConfigServerIni, EveryRoleParses )
{
    EXPECT_EQ(
        atlas::ServerConfig::FromIni( atlas::IniDocument::Parse( MakeIni( "fe", "7777" ) ) ).role,
        atlas::ServerRole::Fe );
    EXPECT_EQ(
        atlas::ServerConfig::FromIni( atlas::IniDocument::Parse( MakeIni( "world", "7777" ) ) )
            .role,
        atlas::ServerRole::World );
    EXPECT_EQ(
        atlas::ServerConfig::FromIni( atlas::IniDocument::Parse( MakeIni( "interworld", "7777" ) ) )
            .role,
        atlas::ServerRole::InterWorld );
}

TEST( ConfigIniDocument, ParsesSectionsCommentsAndBlankLines )
{
    const atlas::IniDocument document = atlas::IniDocument::Parse(
        "; leading comment\n\n[server]\n# another comment\n  role  =  game   ; trailing\n" );

    EXPECT_EQ( document.Get( "server", "role" ).value_or( "<missing>" ), "game" );
    EXPECT_FALSE( document.Get( "server", "absent" ).has_value() );
    EXPECT_FALSE( document.Get( "absent", "role" ).has_value() );
}

// 잘못된 입력은 반드시 실패. static_cast<void> 는 [[nodiscard]] /W4 억제용
TEST( ConfigIniDocument, RejectsMalformedDocuments )
{
    EXPECT_THROW(
        static_cast< void >( atlas::IniDocument::Parse( "[server]\nrole = game\nrole = fe\n" ) ),
        atlas::Exception );
    EXPECT_THROW( static_cast< void >( atlas::IniDocument::Parse( "role = game\n" ) ),
                  atlas::Exception );
    EXPECT_THROW( static_cast< void >( atlas::IniDocument::Parse( "[server\nrole = game\n" ) ),
                  atlas::Exception );
    EXPECT_THROW( static_cast< void >( atlas::IniDocument::LoadFile( "no_such_file_here.ini" ) ),
                  atlas::Exception );
}

TEST( ConfigServerIni, RejectsAnUnknownRole )
{
    EXPECT_THROW( static_cast< void >( atlas::ServerConfig::FromIni(
                      atlas::IniDocument::Parse( MakeIni( "lobby", "1" ) ) ) ),
                  atlas::Exception );
}

// [CS 4.1] listen_port 는 UInt16. 폭 밖의 값은 4464 로 조용히 감기지 않고 실패
TEST( ConfigServerIni, RejectsAListenPortOutsideUInt16 )
{
    EXPECT_THROW( static_cast< void >( atlas::ServerConfig::FromIni(
                      atlas::IniDocument::Parse( MakeIni( "game", "70000" ) ) ) ),
                  atlas::Exception );
    EXPECT_THROW( static_cast< void >( atlas::ServerConfig::FromIni(
                      atlas::IniDocument::Parse( MakeIni( "game", "-1" ) ) ) ),
                  atlas::Exception );
}

TEST( ConfigServerIni, RejectsAMissingRequiredKey )
{
    EXPECT_THROW( static_cast< void >( atlas::ServerConfig::FromIni(
                      atlas::IniDocument::Parse( "[server]\nrole = game\n" ) ) ),
                  atlas::Exception );
}

// [AD 5.4] 로더는 키 이름만 찍음. 자격 증명 유출을 막는 유일한 통제
TEST( ConfigSecrets, NeverPutsASecretValueIntoADiagnosticString )
{
    constexpr const char* kSentinel = "SENTINEL-PASSWORD-9f3a1c";

    SetTestEnv( "ATLAS_DB_HOST", "db.internal" );
    SetTestEnv( "ATLAS_DB_PORT", "3306" );
    SetTestEnv( "ATLAS_DB_NAME", "atlas" );
    SetTestEnv( "ATLAS_DB_USER", "atlas_user" );
    SetTestEnv( "ATLAS_DB_PASSWORD", kSentinel );
    SetTestEnv( "ATLAS_JWKS_URL", "https://auth.example/.well-known/jwks.json" );

    const atlas::SecretConfig secrets = atlas::SecretConfig::FromEnvironment();

    // 양성 대조. 없으면 "값이 안 보임" 과 "읽은 적 없음" 이 구분되지 않음
    EXPECT_EQ( secrets.db_password, kSentinel );
    EXPECT_EQ( secrets.db_port, atlas::UInt16{ 3306 } );

    std::string diagnostics = secrets.DescribePresence();
    SetTestEnv( "ATLAS_DB_PORT", "not-a-port" );
    try
    {
        static_cast< void >( atlas::SecretConfig::FromEnvironment() );
        FAIL() << "a malformed ATLAS_DB_PORT must be rejected";
    }
    catch ( const atlas::Exception& ex )
    {
        diagnostics += ex.what();
    }

    EXPECT_EQ( diagnostics.find( kSentinel ), std::string::npos );
    // 키 이름은 반드시 남아야 함. 그것이 진단으로서의 값어치
    EXPECT_NE( diagnostics.find( "ATLAS_DB_PASSWORD" ), std::string::npos );
    EXPECT_NE( diagnostics.find( "ATLAS_DB_PORT" ), std::string::npos );

    atlas::SetLogLevel( atlas::LogLevel::Info );
    const atlas::UInt64 info_before = atlas::LogCount( atlas::LogLevel::Info );
    secrets.LogSummary();
    EXPECT_EQ( atlas::LogCount( atlas::LogLevel::Info ), info_before + 1 );

    SetTestEnv( "ATLAS_DB_PASSWORD", "" );
    SetTestEnv( "ATLAS_DB_PORT", "" );
}

TEST( ConfigSecrets, MissingKeysAreEmptyRatherThanAFailure )
{
    SetTestEnv( "ATLAS_DB_PASSWORD", "" );
    SetTestEnv( "ATLAS_DB_PORT", "" );

    const atlas::SecretConfig secrets = atlas::SecretConfig::FromEnvironment();
    const std::string summary = secrets.DescribePresence();

    EXPECT_TRUE( secrets.db_password.empty() );
    EXPECT_EQ( secrets.db_port, atlas::UInt16{ 0 } );
    EXPECT_NE( summary.find( "ATLAS_DB_PASSWORD=<empty>" ), std::string::npos );
    EXPECT_NE( summary.find( "ATLAS_DB_PORT=<empty>" ), std::string::npos );
}

}  // namespace
