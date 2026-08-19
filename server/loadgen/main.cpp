// =============================================================================
// 부하 하네스 바이너리. 자기 데이터를 시드하고 실행한 뒤 다시 지움
// =============================================================================

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <future>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include "atlas/config/secret_config.h"
#include "atlas/core/ctx.h"
#include "atlas/core/error.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/db/prepared_statement.h"
#include "atlas/net/io_runner.h"
#include "atlas/redis/connection.h"
#include "game/character_cache.h"
#include "game/inventory.h"
#include "game/ranking.h"
#include "generated/db/character_items_row.h"
#include "generated/db/characters_row.h"
#include "loadgen/load_client.h"

// atlas_loadgen --connections 64 --duration 20 --warmup 3 --io-threads 8
// [AD 16.1i] 램프 축 --ramp / --stage-seconds / --no-delay
// [AD 16.1a] 관측 축 --tui / --sample-jsonl / --probe-file. 전부 기본 off
// [AD 10.5] 시나리오는 connect -> 캐릭터 로드 -> equip 반복. 연결마다 전용 캐릭터
// [AD 16.1h] 실행이 지우는 것은 행과 캐시 사본과 랭킹 항목

namespace
{

using atlas::DbValue;
using atlas::Float64;
using atlas::Int64;
using atlas::UInt16;
using atlas::UInt32;
using atlas::UInt64;
using atlas::UInt8;

// 시드 블록은 server_id 가 아니라 id 범위로 삭제한다
// 하네스는 배포된 GAME 과 같은 server_id 를 쓴다
// 서버 단위 삭제는 만들지 않은 행까지 지운다
constexpr std::string_view kDeleteItemsSql =
    "DELETE FROM `character_items` WHERE `server_id` = ? AND `character_id` >= ? AND "
    "`character_id` < ?";
constexpr std::string_view kDeleteCharactersSql =
    "DELETE FROM `characters` WHERE `server_id` = ? AND `character_id` >= ? AND "
    "`character_id` < ?";

// [AD 6] 시드 캐릭터가 매달리는 계정. 계정 1개에 캐릭터 N개가 문서의 소유 구조
constexpr UInt64 kAccountUid = 990000;

UInt64 ParseUnsigned( std::string_view text )
{
    UInt64 value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const std::from_chars_result parsed = std::from_chars( first, last, value );
    ATLAS_CHECK( parsed.ec == std::errc{} && parsed.ptr == last, "'{}' is not a whole number",
                 text );
    return value;
}

std::vector< std::size_t > ParseStageList( std::string_view text )
{
    std::vector< std::size_t > stages;
    std::size_t begin = 0;
    while ( true )
    {
        const std::size_t comma = text.find( ',', begin );
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        stages.push_back(
            static_cast< std::size_t >( ParseUnsigned( text.substr( begin, end - begin ) ) ) );
        if ( comma == std::string_view::npos )
        {
            break;
        }
        begin = comma + 1;
    }
    return stages;
}

// 이 스키마의 DATETIME 에는 소수부가 없음
atlas::SysTime NowToWholeSecond()
{
    return std::chrono::floor< std::chrono::seconds >( atlas::SysClock::now() );
}

UInt64 FirstItemOf( UInt64 character_id ) { return ( character_id * 10U ) + 1U; }
UInt64 SecondItemOf( UInt64 character_id ) { return ( character_id * 10U ) + 2U; }

void DeleteRange( atlas::Connection& connection, UInt16 server_id, UInt64 first_character,
                  std::size_t count )
{
    const std::array< DbValue, 3 > range{
        DbValue{ static_cast< UInt64 >( server_id ) }, DbValue{ first_character },
        DbValue{ first_character + static_cast< UInt64 >( count ) } };
    connection.Prepare( kDeleteItemsSql ).Execute( range );
    connection.Prepare( kDeleteCharactersSql ).Execute( range );
}

// 접속 대기 상한과 purge 명령 1회의 상한
// 뒤쪽은 atlas/redis 의 명령당 500 ms 마감보다 크다
// 여기서 타임아웃이면 마감이 빡빡한 게 아니라 무응답이다
constexpr atlas::Seconds kRedisReadyBudget{ 2 };
constexpr atlas::Seconds kRedisCommandBudget{ 2 };

// 프로세스 수명 스레드 전용
// 응답이 연결 strand 에 도착할 때까지 블로킹한다
// I/O 스레드에서 부르면 안 된다
bool ExecuteBlocking( atlas::RedisConnection& redis, const atlas::RedisCommand& command )
{
    auto answered = std::make_shared< std::promise< bool > >();
    std::future< bool > settled = answered->get_future();
    redis.Execute( atlas::Ctx{}, command, [answered]( const atlas::RedisResult& result )
                   { answered->set_value( result.ok ); } );

    // 프레임 지역 promise 가 아니라 shared_ptr
    // 대기에 상한이 있어 핸들러가 남은 채 반환되면 지역 promise 는 dangling
    // 분석기는 strand 로 post 된 참조를 추적하지 못해 누수로 읽는다
    // 아래 마커는 바로 앞 한 줄만 덮으므로 사이에 주석을 두면 안 된다
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    return settled.wait_for( kRedisCommandBudget ) == std::future_status::ready && settled.get();
}

// DeleteRange 의 Redis 절반 - 시드 블록의 캐시 사본과 랭킹 멤버.
// SQL 과 같은 이유로 시드 id 범위에만 적용. 키 패턴 스캔도 FLUSHDB 도 없음.
// 명령은 2N 개가 아니라 2개 - DEL 과 ZREM 이 목록 전체를 받음
void PurgeRedisRange( atlas::RedisConnection& redis, UInt16 server_id, UInt64 first_character,
                      std::size_t count )
{
    // [AD 10.2] 캐시는 선택. 없는 실행은 치울 잔재도 없으므로 실패로 보지 않음
    if ( !redis.IsConfigured() )
    {
        return;
    }

    std::vector< std::string > cache_keys;
    cache_keys.reserve( count );
    std::vector< std::string > ranking_args;
    ranking_args.reserve( count + 1U );
    ranking_args.push_back( atlas_demo::ExpRankingKey( server_id ) );
    for ( std::size_t index = 0; index < count; ++index )
    {
        const UInt64 character_id = first_character + static_cast< UInt64 >( index );
        cache_keys.push_back( atlas_demo::CharacterCacheKey(
            server_id, static_cast< atlas::CharacterId >( character_id ) ) );
        ranking_args.push_back( std::to_string( character_id ) );
    }

    const bool cache_ok = ExecuteBlocking(
        redis, atlas::RedisCommand{ .verb = "DEL", .args = std::move( cache_keys ) } );
    const bool ranking_ok = ExecuteBlocking(
        redis, atlas::RedisCommand{ .verb = "ZREM", .args = std::move( ranking_args ) } );
    if ( !cache_ok || !ranking_ok )
    {
        // 로그가 아니라 출력 - 운영자에게 하는 말이며 수치에는 영향 없음.
        // 잔재가 프로세스보다 오래 남으므로 크게 알림
        std::printf(
            "WARNING - redis cleanup incomplete (cache=%s ranking=%s). Seeded cache keys "
            "or ranking entries may survive this run.\n",
            cache_ok ? "ok" : "FAILED", ranking_ok ? "ok" : "FAILED" );
    }
}

void Seed( atlas::Connection& connection, UInt16 server_id, UInt64 first_character,
           std::size_t count )
{
    for ( std::size_t index = 0; index < count; ++index )
    {
        const UInt64 character_id = first_character + static_cast< UInt64 >( index );
        const std::array< DbValue, 10 > character{
            DbValue{ static_cast< UInt64 >( server_id ) },
            DbValue{ character_id },
            DbValue{ kAccountUid },
            DbValue{ "load-" + std::to_string( character_id ) },
            DbValue{ Int64{ 0 } },
            DbValue{ Int64{ 0 } },
            DbValue{ UInt64{ 1 } },
            DbValue{ UInt64{ 0 } },
            DbValue{ NowToWholeSecond() },
            DbValue{ std::monostate{} } };
        connection.Prepare( atlas::generated::kCharactersInsertSql ).Execute( character );

        // [AD 10.5] 첫 아이템은 대상 슬롯에 착용된 채로 시작한다
        // 그래야 매 요청이 실제 점유자를 해제하고 쓰기 3회가 모두 실행된다
        const std::array< DbValue, 6 > worn{
            DbValue{ static_cast< UInt64 >( server_id ) }, DbValue{ character_id },
            DbValue{ FirstItemOf( character_id ) },
            // [AD 8.2] 1001/1002 는 item.csv 의 slot 1 아이템
            // equip 경로는 정적 데이터에 없는 id 와 슬롯 불일치를 거절한다
            // 임의 id 를 시드하면 3회 쓰기가 아니라 거절 경로를 측정하게 된다
            DbValue{ UInt64{ 1001 } }, DbValue{ UInt64{ 1 } },
            DbValue{
                static_cast< UInt64 >( static_cast< UInt8 >( atlas_demo::EquipSlot::Weapon ) ) } };
        connection.Prepare( atlas::generated::kCharacterItemsInsertSql ).Execute( worn );

        const std::array< DbValue, 6 > spare{ DbValue{ static_cast< UInt64 >( server_id ) },
                                              DbValue{ character_id },
                                              DbValue{ SecondItemOf( character_id ) },
                                              DbValue{ UInt64{ 1002 } },
                                              DbValue{ UInt64{ 1 } },
                                              DbValue{ static_cast< UInt64 >( static_cast< UInt8 >(
                                                  atlas_demo::EquipSlot::None ) ) } };
        connection.Prepare( atlas::generated::kCharacterItemsInsertSql ).Execute( spare );
    }
}

void Report( const atlas_loadgen::LoadOptions& options, const atlas_loadgen::LoadStats& stats )
{
    std::vector< UInt32 > samples = stats.latencies_us;
    std::ranges::sort( samples );

    const Float64 window_seconds = static_cast< Float64 >( stats.steady_window_ms ) / 1000.0;
    const Float64 throughput =
        window_seconds > 0.0 ? static_cast< Float64 >( samples.size() ) / window_seconds : 0.0;
    const UInt64 total_us =
        std::accumulate( samples.begin(), samples.end(), UInt64{ 0 },
                         []( UInt64 sum, UInt32 sample ) { return sum + sample; } );
    const Float64 mean_us = samples.empty() ? 0.0
                                            : static_cast< Float64 >( total_us ) /
                                                  static_cast< Float64 >( samples.size() );

    std::printf( "connections=%zu rate=%u duration_s=%u warmup_s=%u io_threads=%zu no_delay=%s\n",
                 options.connections, options.rate_per_second, options.duration_seconds,
                 options.warmup_seconds, options.io_threads,
                 options.no_delay.has_value() ? ( *options.no_delay ? "1" : "0" ) : "unset" );
    if ( stats.no_delay_failures > 0 )
    {
        // 라벨과 소켓의 실제 동작이 다르므로 크게 알림
        std::printf(
            "TCP_NODELAY REQUEST FAILED on %zu connections - this cell is not what it says\n",
            stats.no_delay_failures );
    }
    std::printf(
        "established=%zu peak_live=%zu connect_fail=%zu transport_fail=%zu load_fail=%zu\n",
        stats.connections_established, stats.peak_live_connections, stats.connect_failures,
        stats.transport_failures, stats.load_failures );
    std::printf( "requests_sent=%zu ok=%zu unavailable=%zu refused=%zu load_rejected=%zu\n",
                 stats.requests_sent, stats.responses_ok, stats.responses_unavailable,
                 stats.responses_refused, stats.loads_rejected );
    std::printf( "steady_window_ms=%u samples=%zu throughput_rps=%.1f\n", stats.steady_window_ms,
                 samples.size(), throughput );
    std::printf(
        "mean_us=%.0f p50_us=%u p90_us=%u p99_us=%u p999_us=%u max_us=%u\n", mean_us,
        atlas_loadgen::Percentile( samples, 0.50 ), atlas_loadgen::Percentile( samples, 0.90 ),
        atlas_loadgen::Percentile( samples, 0.99 ), atlas_loadgen::Percentile( samples, 0.999 ),
        atlas_loadgen::Percentile( samples, 1.0 ) );
    // 램프일 때만 출력한다
    // 고정 동시성의 단일 단계는 위 블록 그 자체다
    // 두 번 찍으면 그 반복이 두 번째 측정으로 읽힌다
    if ( stats.stages.size() > 1 )
    {
        std::printf(
            "\n--- per stage --- (the aggregate above spans every stage and is therefore an "
            "average, not a capacity)\n" );
        for ( std::size_t index = 0; index < stats.stages.size(); ++index )
        {
            const atlas_loadgen::StageStats& stage = stats.stages[index];
            std::vector< UInt32 > stage_samples = stage.latencies_us;
            std::ranges::sort( stage_samples );
            const Float64 stage_seconds = static_cast< Float64 >( stage.window_ms ) / 1000.0;
            const Float64 stage_throughput =
                stage_seconds > 0.0 ? static_cast< Float64 >( stage_samples.size() ) / stage_seconds
                                    : 0.0;
            const std::size_t answered =
                stage.responses_ok + stage.responses_rejected + stage.responses_refused;
            // 분모와 같은 줄의 거부 "비율". 700 중 600 과 60000 중 600 은 정반대
            const Float64 reject_percent =
                answered > 0 ? ( static_cast< Float64 >( stage.responses_rejected ) * 100.0 ) /
                                   static_cast< Float64 >( answered )
                             : 0.0;
            std::vector< UInt32 > ok_samples = stage.ok_latencies_us;
            std::ranges::sort( ok_samples );
            const Float64 ok_throughput =
                stage_seconds > 0.0 ? static_cast< Float64 >( ok_samples.size() ) / stage_seconds
                                    : 0.0;
            std::printf(
                "stage=%zu connections=%zu window_ms=%u samples=%zu throughput_rps=%.1f ok=%zu "
                "ok_rps=%.1f rejected=%zu reject_pct=%.2f refused=%zu p50_us=%u p90_us=%u "
                "p99_us=%u max_us=%u ok_p50_us=%u ok_p90_us=%u ok_p99_us=%u ok_max_us=%u\n",
                index, stage.connections, stage.window_ms, stage_samples.size(), stage_throughput,
                stage.responses_ok, ok_throughput, stage.responses_rejected, reject_percent,
                stage.responses_refused, atlas_loadgen::Percentile( stage_samples, 0.50 ),
                atlas_loadgen::Percentile( stage_samples, 0.90 ),
                atlas_loadgen::Percentile( stage_samples, 0.99 ),
                atlas_loadgen::Percentile( stage_samples, 1.0 ),
                atlas_loadgen::Percentile( ok_samples, 0.50 ),
                atlas_loadgen::Percentile( ok_samples, 0.90 ),
                atlas_loadgen::Percentile( ok_samples, 0.99 ),
                atlas_loadgen::Percentile( ok_samples, 1.0 ) );
        }
    }
    if ( stats.watchdog_fired )
    {
        // 위 수치 전부가 의심 대상이 된다
        // 돌아오지 않은 연결은 표본을 내지 않는다
        // 그래서 백분위가 살아남은 요청만 설명하게 된다
        std::printf(
            "WATCHDOG FIRED - at least one connection never completed. Treat the "
            "percentiles above as a lower bound.\n" );
    }
}

}  // namespace

int main( int argc, char** argv )
{  // NOLINT - main 시그니처는 표준이 정함
    try
    {
        // 콘솔만, Warn 이상. 측정 중 디스크를 놓고 경쟁할 로그 파일을 늘리지 않음
        atlas::LogConfig log_config;
        log_config.level = atlas::LogLevel::Warn;
        atlas::LogInit( log_config );

        atlas_loadgen::LoadOptions options;
        const auto argument_count = static_cast< std::size_t >( argc );
        for ( std::size_t index = 1; index < argument_count; ++index )
        {
            const std::string_view key( argv[index] );
            // 값이 없는 유일한 옵션이라 아래 짝 규칙보다 먼저 처리
            if ( key == "--tui" )
            {
                options.tui = true;
                continue;
            }
            ATLAS_CHECK( index + 1 < argument_count, "option '{}' needs a value", key );
            const std::string_view value( argv[index + 1] );
            ++index;
            if ( key == "--host" )
            {
                options.host = std::string( value );
            }
            else if ( key == "--port" )
            {
                options.port = static_cast< UInt16 >( ParseUnsigned( value ) );
            }
            else if ( key == "--connections" )
            {
                options.connections = static_cast< std::size_t >( ParseUnsigned( value ) );
            }
            else if ( key == "--rate" )
            {
                options.rate_per_second = static_cast< UInt32 >( ParseUnsigned( value ) );
            }
            else if ( key == "--duration" )
            {
                options.duration_seconds = static_cast< UInt32 >( ParseUnsigned( value ) );
            }
            else if ( key == "--warmup" )
            {
                options.warmup_seconds = static_cast< UInt32 >( ParseUnsigned( value ) );
            }
            else if ( key == "--io-threads" )
            {
                options.io_threads = static_cast< std::size_t >( ParseUnsigned( value ) );
            }
            else if ( key == "--server-id" )
            {
                options.server_id = static_cast< UInt16 >( ParseUnsigned( value ) );
            }
            else if ( key == "--first-character" )
            {
                options.first_character_id = ParseUnsigned( value );
            }
            else if ( key == "--no-delay" )
            {
                options.no_delay = ParseUnsigned( value ) != 0;
            }
            else if ( key == "--ramp" )
            {
                options.ramp_stages = ParseStageList( value );
            }
            else if ( key == "--stage-seconds" )
            {
                options.stage_seconds = static_cast< UInt32 >( ParseUnsigned( value ) );
            }
            else if ( key == "--sample-jsonl" )
            {
                options.sample_jsonl_path = std::string( value );
            }
            else if ( key == "--probe-file" )
            {
                options.probe_file_path = std::string( value );
            }
            else
            {
                ATLAS_THROW( atlas::Exception, "unknown option '{}'", key );
            }
        }
        if ( !options.ramp_stages.empty() )
        {
            ATLAS_CHECK( options.stage_seconds > 0, "--ramp needs --stage-seconds" );
            // [AD 16.1a] closed loop 전용
            // open loop 램프는 단계 경계마다 연결별 간격을 다시 잡아야 한다
            // 어긋나면 하네스가 자기 버스트를 서버 지연으로 보고한다
            // 한 번 겪은 thundering herd 가 그대로 되살아난다
            ATLAS_CHECK( options.rate_per_second == 0,
                         "--ramp is closed loop only; drop --rate or drop --ramp" );
            ATLAS_CHECK( options.warmup_seconds < options.stage_seconds,
                         "--warmup is the per-stage discarded prefix in ramp mode, so it must be "
                         "shorter than --stage-seconds" );
            ATLAS_CHECK( options.ramp_stages.front() > 0, "--ramp stages must be at least 1" );
            for ( std::size_t index = 1; index < options.ramp_stages.size(); ++index )
            {
                // 연결은 합류만 하고 빠지지 않아 감소 단계는 지킬 수 없다
                // 조용히 큰 값을 쓰면 적용하지 않은 램프를 보고하게 된다
                ATLAS_CHECK( options.ramp_stages[index] > options.ramp_stages[index - 1],
                             "--ramp must be strictly increasing" );
            }
            options.connections = options.ramp_stages.back();
            options.duration_seconds =
                options.stage_seconds * static_cast< UInt32 >( options.ramp_stages.size() );
        }
        ATLAS_CHECK( options.connections > 0, "--connections must be at least 1" );
        ATLAS_CHECK( options.warmup_seconds < options.duration_seconds,
                     "--warmup must be shorter than --duration" );

        const atlas::SecretConfig secrets = atlas::SecretConfig::FromEnvironment();
        ATLAS_CHECK(
            !secrets.db_host.empty() && !secrets.db_name.empty() && !secrets.db_user.empty(),
            "the database secrets are incomplete; see server/.env.example" );

        atlas::DbConnectionConfig db_config;
        db_config.host = secrets.db_host;
        db_config.port = secrets.db_port == 0 ? UInt16{ 3306 } : secrets.db_port;
        db_config.database = secrets.db_name;
        db_config.user = secrets.db_user;
        db_config.password = secrets.db_password;
        db_config.tls_no_verify = secrets.db_tls_no_verify;
#if defined( ATLAS_MARIADB_PLUGIN_DIR )
        db_config.plugin_directory = ATLAS_MARIADB_PLUGIN_DIR;
#endif

        atlas::RedisConnectionConfig redis_config;
        redis_config.host = secrets.redis_host;
        redis_config.port = secrets.redis_port == 0 ? UInt16{ 6379 } : secrets.redis_port;
        redis_config.password = secrets.redis_password;
        // .env 에는 compose 서비스명이 들어 있다
        // 이 하네스는 항상 호스트에서 도는 클라이언트라 그 이름이 통하지 않는다
        // Redis 쪽은 밖에서 바꿔 주는 것이 없다
        // [AD 10.2] 미설정 호스트는 그대로 둔다. 캐시 off 이지 오타 아님
        if ( redis_config.host == "redis" )
        {
            redis_config.host = "127.0.0.1";
        }

        // [AD 9] io runner 보다 먼저 정지한다
        // 실행 루프가 미완 작업인 상태에서 work guard 를 놓으면 컨텍스트가 안 비워진다
        // 선언 순서가 그것을 보장한다
        atlas::IoRunner redis_io( std::size_t{ 1 } );
        atlas::RedisConnection redis( redis_io.Context(), redis_config );
        if ( redis.IsConfigured() )
        {
            redis_io.Start();
            redis.Start();
            if ( !redis.WaitUntilReady( kRedisReadyBudget ) )
            {
                std::printf(
                    "WARNING - redis is configured at %s but did not answer; this run's "
                    "cache and ranking cleanup will not happen.\n",
                    redis_config.host.c_str() );
            }
        }

        atlas::Connection seeder( db_config );
        // 먼저 삭제. 죽은 이전 실행이 블록을 남기면 INSERT 가 기본키에서 실패한다
        // 캐시와 랭킹은 이유가 더 강하다
        // id 를 재사용하므로 이전 실행의 사본이 이번 실행 캐릭터를 설명하게 된다
        DeleteRange( seeder, options.server_id, options.first_character_id, options.connections );
        PurgeRedisRange( redis, options.server_id, options.first_character_id,
                         options.connections );
        Seed( seeder, options.server_id, options.first_character_id, options.connections );
        std::printf( "seeded %zu characters (server_id=%u, character_id %llu..%llu)\n",
                     options.connections, options.server_id,
                     static_cast< unsigned long long >( options.first_character_id ),
                     static_cast< unsigned long long >( options.first_character_id +
                                                        options.connections - 1 ) );

        const atlas_loadgen::LoadStats stats = atlas_loadgen::RunLoad( options );
        Report( options, stats );

        DeleteRange( seeder, options.server_id, options.first_character_id, options.connections );
        PurgeRedisRange( redis, options.server_id, options.first_character_id,
                         options.connections );
        std::printf( "seeded rows removed\n" );

        redis.Stop();
        redis_io.Stop();
        atlas::LogShutdown();
        return 0;
    }
    catch ( const std::exception& failure )
    {
        std::printf( "loadgen failed: %s\n", failure.what() );
        atlas::LogShutdown();
        return 70;
    }
}
