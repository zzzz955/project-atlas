// =============================================================================
// [AD 5.1] GAME 서버 바이너리. 단일 이미지 - 역할 분기 진입점
// [AD 15.3] fe / world 는 크게 실패해야 함. 배선은 handlers.cpp 에 있음
// =============================================================================

#include <atomic>
#include <csignal>
#include <cstddef>
#include <exception>
#include <string>
#include <thread>

#include "atlas/config/secret_config.h"
#include "atlas/config/server_config.h"
#include "atlas/core/log.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/net/net_types.h"
#include "atlas/redis/connection.h"
#include "game/handlers.h"

namespace
{

// 우아한 종료 시그널 핸들러가 하는 일은 플래그 저장뿐
// [AD 9] SIGINT/SIGTERM 은 순서 있는 종료로 가야 함
// 여기서 로거 작업을 하지 않는 것이 async-signal-safe 를 유지함
std::atomic< bool > g_stop_requested{
    false };  // NOLINT - 시그널 플래그는 네임스페이스 범위에 있어야 함

extern "C" void OnStopSignal( int /*signal_number*/ ) { g_stop_requested.store( true ); }

// server.ini 위치는 argv[1], 없으면 작업 디렉터리
// [AD 5.4] 환경 변수 재정의를 하나 더 두면 같은 답이 두 곳에 생김
std::string IniPath( const char* argument )
{
    return argument == nullptr ? std::string( "server.ini" ) : std::string( argument );
}

// [AD 10.3] server.ini 에서 읽지 않음
// 타입 있는 ini 뷰를 이 슬라이스에서 넓히지 않았음
// 아무도 파싱하지 않는 키를 커밋하면 장식일 뿐
constexpr std::size_t kDbPoolSize = 4;
constexpr std::size_t kDbThreads = 2;

// [AD 16.1] 서버가 자기 카운터를 말하는 주기
// [AD 16.1g] 5초에 INFO 한 줄. 요청마다 찍는 로그는 실측된 서버측 비용
// 부하를 왜곡하는 계수기는 없느니만 못함
// 카운터는 누적이라 표본 사이 손실 없음
constexpr atlas::Seconds kCounterInterval{ 5 };

}  // namespace

int main( int argc, char** argv )
{  // NOLINT - 표준이 main 의 시그니처를 고정함
    try
    {
        const atlas::ServerConfig config =
            atlas::ServerConfig::LoadFile( IniPath( argc > 1 ? argv[1] : nullptr ) );

        atlas::LogConfig log_config;
        log_config.directory = config.log.dir;
        log_config.basename = "atlas_game";
        log_config.level = config.log.level;
        log_config.retain_days = static_cast< atlas::UInt16 >( config.log.retention_days );
        atlas::LogInit( log_config );

        // [AD 5.4] ATLAS_ROLE 이 바이너리를 고르고 [server] role 은 런타임 신원
        // 둘은 일치해야 함
        // 아무도 고르지 않은 신원으로 뜨는 서버는 안 뜨는 서버보다 나쁨
        if ( config.role != atlas::ServerRole::Game )
        {
            ATLAS_LOG_FATAL( "this is the GAME binary but server.ini says role={}",
                             atlas::RoleName( config.role ) );
            atlas::LogShutdown();
            return 64;
        }

        const atlas::SecretConfig secrets = atlas::SecretConfig::FromEnvironment();
        // [AD 5.4] 키 이름과 설정 여부만. 값은 절대 로그하지 않음
        secrets.LogSummary();
        if ( secrets.db_host.empty() || secrets.db_name.empty() || secrets.db_user.empty() )
        {
            ATLAS_LOG_FATAL( "the database secrets are incomplete; see server/.env.example" );
            atlas::LogShutdown();
            return 78;
        }

        atlas::DbConnectionConfig db_config;
        db_config.host = secrets.db_host;
        db_config.port = secrets.db_port == 0 ? atlas::UInt16{ 3306 } : secrets.db_port;
        db_config.database = secrets.db_name;
        db_config.user = secrets.db_user;
        db_config.password = secrets.db_password;
        db_config.tls_no_verify = secrets.db_tls_no_verify;
        if ( db_config.tls_no_verify )
        {
            // 로그에 흔적이 없는 완화된 TLS 는 실수와 구분되지 않음
            // 그래서 부팅마다 한 번 WARN 으로 스스로를 알림
            ATLAS_LOG_WARN(
                "ATLAS_DB_TLS_NO_VERIFY=1 - the database connection is encrypted but the server "
                "certificate is NOT verified. Local/compose only; never production." );
        }

        // [AD 10.2] 캐시는 선택. 없으면 경고이지 실패가 아님
        // 미설정도 조회 실패도 결국 DB 로 가는 한 경로
        atlas::RedisConnectionConfig redis_config;
        redis_config.host = secrets.redis_host;
        redis_config.port = secrets.redis_port == 0 ? atlas::UInt16{ 6379 } : secrets.redis_port;
        redis_config.password = secrets.redis_password;
        if ( redis_config.host.empty() )
        {
            ATLAS_LOG_WARN(
                "ATLAS_REDIS_HOST is unset - the character read cache and the exp ranking are off "
                "and every character load goes to the database. See server/.env.example." );
        }

        atlas_demo::GameServer::Options options;
        options.endpoint = atlas::Endpoint( atlas::Tcp::v4(), config.listen_port );
        options.server_id = static_cast< atlas::UInt16 >( config.server_id );
        options.io_workers = static_cast< std::size_t >( config.io_workers );
        options.db_pool_size = kDbPoolSize;
        options.db_threads = kDbThreads;

        atlas_demo::GameServer server( options, db_config, redis_config );

        // 우아한 종료는 crash.cpp 의 치명 시그널 경로와 별개. 덤프가 아니라 배수
        std::signal( SIGINT, &OnStopSignal );
        std::signal( SIGTERM, &OnStopSignal );

        server.Start();

        // asio signal_set 대신 폴링
        // 핸들러가 워커 스레드에서 돌면 워커를 join 하는 Stop() 이 자기를 join 함
        atlas::TimePoint next_counter_log = atlas::Clock::now() + kCounterInterval;
        while ( !g_stop_requested.load() )
        {
            std::this_thread::sleep_for( atlas::Millis{ 200 } );
            const atlas::TimePoint now = atlas::Clock::now();
            if ( now < next_counter_log )
            {
                continue;
            }
            next_counter_log = now + kCounterInterval;
            const atlas_demo::GameServer::Counters counters = server.ReadCounters();
            // 큐 깊이를 상한과 함께 찍음
            // 40 은 어떤 상한에서는 한가하고 다른 상한에서는 셰딩 직전
            // 상한 도달 여부는 db_rejected 가 말함
            ATLAS_LOG_INFO(
                "counters sessions={} db_queue={}/{} db_rejected={} db_acquire_timeouts={} "
                "db_acquire_wait_us={}",
                counters.live_sessions, counters.db_queue_depth, counters.db_queue_capacity,
                counters.db_rejected, counters.db_acquire_failures,
                counters.db_acquire_wait_micros );
        }

        ATLAS_LOG_INFO( "stop requested - draining" );
        server.Stop();
        ATLAS_LOG_INFO( "GAME stopped" );
        atlas::LogShutdown();
        return 0;
    }
    catch ( const std::exception& failure )
    {
        // 기동 실패가 여기로 옴. 포트 선점 - 자격 거부 - 잘못된 ini
        // 전부 저하가 아니라 크고 치명적이어야 함
        ATLAS_LOG_FATAL( "GAME failed to start: {}", failure.what() );
        atlas::LogShutdown();
        return 70;
    }
}
