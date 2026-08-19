// =============================================================================
// Connection 구현. 드라이버 옵션 협상과 재접속, 트랜잭션 제어
// =============================================================================

#include "atlas/db/connection.h"

#include <mysql.h>

#include <memory>
#include <string>
#include <utility>

#include "atlas/core/log.h"
#include "atlas/db/prepared_statement.h"

namespace atlas
{

Connection::Connection( DbConnectionConfig config ) : config_( std::move( config ) ) { Open(); }

void Connection::Open()
{
    const DbConnectionConfig& config = config_;
    handle_ = mysql_init( nullptr );
    if ( handle_ == nullptr )
    {
        ATLAS_THROW( DbException, "mysql_init failed" );
    }

    // 드라이버 자체 재접속은 OFF 로 못박음
    // 기본값이지만 아래 전부가 여기 기대므로 가정하지 않고 명시
    // mysql_ping 의 자동 재접속은 죽은 소켓의 MYSQL_STMT 를 쥔 채 소켓만 되살림
    my_bool reconnect = 0;
    mysql_options( handle_, MYSQL_OPT_RECONNECT, &reconnect );

    // 스키마가 utf8mb4
    // 서버 기본값에 기대지 않고 여기서 협상해야 이름 컬럼이 바이트 그대로 왕복
    mysql_options( handle_, MYSQL_SET_CHARSET_NAME, "utf8mb4" );

    // DbConnectionConfig::plugin_directory 참고
    // 없으면 핸드셰이크가 "plugin not found" 가 아니라 "access denied" 로 실패
    if ( !config.plugin_directory.empty() )
    {
        mysql_options( handle_, MYSQL_PLUGIN_DIR, config.plugin_directory.c_str() );
    }

    if ( config.tls_no_verify )
    {
        my_bool enforce = 1;
        my_bool verify = 0;
        // 두 플래그 다 필요하고 ENFORCE 는 선택이 아님
        // verify 만 끄면 Connector/C 3.4.8 이 TLS 를 포기하고 평문으로 붙음
        // 그때 mysql_get_ssl_cipher 는 nullptr
        // "확인 안 함" 이 조용히 "암호화 안 함" 이 되는 다운그레이드를 막음
        mysql_options( handle_, MYSQL_OPT_SSL_ENFORCE, &enforce );
        mysql_options( handle_, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify );
    }

    if ( mysql_real_connect( handle_, config.host.c_str(), config.user.c_str(),
                             config.password.c_str(), config.database.c_str(), config.port, nullptr,
                             0 ) == nullptr )
    {
        // [AD 5.4] 드라이버 메시지를 일부러 넣지 않음
        // 실패한 핸드셰이크는 "Access denied for user 'x'@'host'" 를 돌려줌
        // host/user 는 .env 출처
        // 숫자 코드면 운영자에게 충분하고 새는 것이 없음(1045 거부, 2003 도달 불가)
        const UInt32 code = mysql_errno( handle_ );
        mysql_close( handle_ );
        handle_ = nullptr;
        ATLAS_THROW( DbException, "db connect failed: driver error {}", code );
    }
}

Connection::~Connection()
{
    // 문장 먼저. MYSQL_STMT 는 prepare 된 MYSQL 에 속함
    // 그 아래에서 연결을 닫으면 드라이버 안의 use-after-free
    // 멤버 소멸 순서가 우연히 맞아도 기댈 보장은 아님
    statements_.clear();
    if ( handle_ != nullptr )
    {
        mysql_close( handle_ );
        handle_ = nullptr;
    }
}

bool Connection::EnsureAlive()
{
    // 풀이 곧 대여할 소켓에 왕복 1회
    // 대여마다 쓰는 그 왕복 하나가 사는 것은 실패 범위의 차이
    // "재시작 동안 몇 요청" 이냐 "누가 고칠 때까지 전 요청" 이냐
    if ( handle_ != nullptr && mysql_ping( handle_ ) == 0 )
    {
        return true;
    }

    // 캐시 먼저, 무조건
    // 여기 MYSQL_STMT 는 방금 죽은 소켓에서 prepare 된 것
    // 하나라도 넘기면 보고되는 에러가 아니라 use-after-free
    statements_.clear();
    if ( handle_ != nullptr )
    {
        mysql_close( handle_ );
        handle_ = nullptr;
    }

    try
    {
        Open();
    }
    catch ( const DbException& ex )
    {
        // [AD 11.2a] 예상된 실패라 다시 던지지 않고 로그 + false
        // 연결은 열리지 않은 채 남고 다음 대여가 다시 시도
        // 여기 루프가 아니라 그것이 재시도
        ATLAS_LOG_WARN( "db connection reconnect failed: {}", ex.what() );
        return false;
    }

    // INFO 가 아니라 WARN. 복구됐어도 운영자는 이 일의 빈도를 봐야 함
    ATLAS_LOG_WARN( "db connection re-established after the server closed it" );
    return true;
}

PreparedStatement& Connection::Prepare( std::string_view sql )
{
    // EnsureAlive 가 false 를 준 뒤에도 연결을 쓴 경우에만 도달
    // 예상된 실패가 아니라 프로그래밍 오류라 던짐
    // 대안은 드라이버에 null 핸들 전달
    ATLAS_CHECK( handle_ != nullptr, "db connection is not open" );

    std::string key( sql );
    const auto found = statements_.find( key );
    if ( found != statements_.end() )
    {
        return *found->second;
    }

    auto statement = std::make_unique< PreparedStatement >( handle_, key );
    PreparedStatement& created = *statement;
    statements_.emplace( std::move( key ), std::move( statement ) );
    ++prepare_count_;
    return created;
}

void Connection::Begin()
{
    // 전용 호출만 쓰고 "START TRANSACTION" 을 mysql_query 로 보내지 않음
    // 텍스트를 아예 안 실어야 "SQL 을 만들지 않는다" 가 트랜잭션 제어까지 성립
    if ( mysql_autocommit( handle_, 0 ) != 0 )
    {
        ATLAS_THROW( DbException, "db begin failed: {}", mysql_error( handle_ ) );
    }
}

void Connection::Commit()
{
    const bool committed = mysql_commit( handle_ ) == 0;
    const std::string reason = committed ? std::string{} : std::string( mysql_error( handle_ ) );
    // 양쪽 경로에서 복구
    // autocommit 이 꺼진 채 돌아간 연결은 무관한 다음 문장에 트랜잭션을 엶
    RestoreAutoCommit();
    if ( !committed )
    {
        ATLAS_THROW( DbException, "db commit failed: {}", reason );
    }
}

void Connection::Rollback() noexcept
{
    // [AD 10] noexcept 인 이유 - 호출자가 예외를 풀고 있는 소멸자
    // 그 풀림 중에 던지면 프로세스가 죽음
    // 실패한 롤백은 로그만 남김
    if ( mysql_rollback( handle_ ) != 0 )
    {
        // core/error.h 의 Guarded::Fail 과 같은 모양
        // noexcept 함수는 보고 경로도 던지지 않아야 함
        // std::format 은 호출 지점에서 돌고 던질 수 있음
        try
        {
            ATLAS_LOG_ERROR( "db rollback failed: {}", mysql_error( handle_ ) );
        }
        catch ( ... )
        {  // NOLINT - 더는 보고할 곳이 없음
        }
    }
    RestoreAutoCommit();
}

void Connection::RestoreAutoCommit() noexcept
{
    if ( mysql_autocommit( handle_, 1 ) != 0 )
    {
        try
        {
            ATLAS_LOG_ERROR( "db autocommit restore failed: {}", mysql_error( handle_ ) );
        }
        catch ( ... )
        {  // NOLINT - Rollback 참고
        }
    }
}

}  // namespace atlas
