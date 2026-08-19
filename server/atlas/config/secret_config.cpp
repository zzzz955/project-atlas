// =============================================================================
// .env 키를 프로세스 환경에서 읽음. 값이 밖으로 새지 않는 경로만 사용
// =============================================================================

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

namespace atlas
{
namespace
{

// [CS 5] MSVC 는 std::getenv 를 막고 소유권을 넘기는 _dupenv_s 를 줌
std::optional< std::string > ReadSecretEnv( const char* name )
{
#if defined( _MSC_VER )
    char* raw = nullptr;
    std::size_t size = 0;
    if ( _dupenv_s( &raw, &size, name ) != 0 || raw == nullptr )
    {
        return std::nullopt;
    }
    std::string value( raw );
    std::free( raw );
    return value;
#else
    const char* raw = std::getenv( name );
    if ( raw == nullptr )
    {
        return std::nullopt;
    }
    return std::string( raw );
#endif
}

std::string ReadSecretString( const char* name )
{
    return ReadSecretEnv( name ).value_or( std::string{} );
}

// 미설정이면 0. 입력을 되뇌는 실패 메시지가 자격 증명을 로그에 남김
UInt16 ReadSecretPort( const char* name )
{
    const std::string port = ReadSecretString( name );
    if ( port.empty() )
    {
        return 0;
    }
    UInt16 value{ 0 };
    const std::from_chars_result result =
        std::from_chars( port.data(), port.data() + port.size(), value );
    ATLAS_CHECK( result.ec == std::errc{} && result.ptr == port.data() + port.size(),
                 "{} is not a valid port number", name );
    return value;
}

void AppendSecretPresence( std::string& out, std::string_view name, bool present )
{
    if ( !out.empty() )
    {
        out += ", ";
    }
    out += name;
    out += present ? "=<set>" : "=<empty>";
}

}  // namespace

SecretConfig SecretConfig::FromEnvironment()
{
    SecretConfig config;
    config.db_host = ReadSecretString( "ATLAS_DB_HOST" );
    config.db_name = ReadSecretString( "ATLAS_DB_NAME" );
    config.db_user = ReadSecretString( "ATLAS_DB_USER" );
    config.db_password = ReadSecretString( "ATLAS_DB_PASSWORD" );
    config.jwks_url = ReadSecretString( "ATLAS_JWKS_URL" );
    // 정확히 "1" 만. 인증서 검증 해제가 "true"/"yes"/공백으로 열리면 안 됨
    config.db_tls_no_verify = ReadSecretString( "ATLAS_DB_TLS_NO_VERIFY" ) == "1";
    config.db_port = ReadSecretPort( "ATLAS_DB_PORT" );

    config.redis_host = ReadSecretString( "ATLAS_REDIS_HOST" );
    config.redis_password = ReadSecretString( "ATLAS_REDIS_PASSWORD" );
    config.redis_port = ReadSecretPort( "ATLAS_REDIS_PORT" );

    return config;
}

std::string SecretConfig::DescribePresence() const
{
    std::string summary;
    AppendSecretPresence( summary, "ATLAS_DB_HOST", !db_host.empty() );
    AppendSecretPresence( summary, "ATLAS_DB_PORT", db_port != 0 );
    AppendSecretPresence( summary, "ATLAS_DB_NAME", !db_name.empty() );
    AppendSecretPresence( summary, "ATLAS_DB_USER", !db_user.empty() );
    AppendSecretPresence( summary, "ATLAS_DB_PASSWORD", !db_password.empty() );
    AppendSecretPresence( summary, "ATLAS_JWKS_URL", !jwks_url.empty() );
    AppendSecretPresence( summary, "ATLAS_REDIS_HOST", !redis_host.empty() );
    AppendSecretPresence( summary, "ATLAS_REDIS_PORT", redis_port != 0 );
    AppendSecretPresence( summary, "ATLAS_REDIS_PASSWORD", !redis_password.empty() );
    return summary;
}

void SecretConfig::LogSummary() const
{
    // 이 파일의 유일한 로그. DescribePresence() 그대로여야 테스트로 고정 가능
    ATLAS_LOG_INFO( "secrets loaded: {}", DescribePresence() );
}

}  // namespace atlas
