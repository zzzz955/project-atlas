// =============================================================================
// 대화형 콘솔 클라이언트 바이너리 - atlas_console --host H --port P
// 명령 5개 전부 실제 프레임. 클라이언트가 지어낸 응답은 없음
// =============================================================================

#include <charconv>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atlas/core/error.h"
#include "atlas/core/log.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/net/io_runner.h"
#include "atlas/net/net_types.h"
#include "console_client/console_client.h"

namespace
{

using atlas::UInt16;
using atlas::UInt64;
using atlas::UInt8;

// 왕복 1회 대기 상한. 로컬 스택에서는 수 ms 라 10초는 확실한 이상 신호.
// 터미널을 매달아 두면 그 사실을 보고하는 대신 감추게 됨
constexpr atlas::Seconds kRequestTimeout{ 10 };

constexpr UInt16 kDefaultRankCount = 10;

[[nodiscard]] std::optional< UInt64 > ParseUnsigned( std::string_view text )
{
    UInt64 value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const std::from_chars_result parsed = std::from_chars( first, last, value );
    if ( parsed.ec != std::errc{} || parsed.ptr != last )
    {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::vector< std::string_view > Split( const std::string& line )
{
    std::vector< std::string_view > tokens;
    const std::string_view view( line );
    std::size_t cursor = 0;
    while ( cursor < view.size() )
    {
        const std::size_t begin = view.find_first_not_of( " \t\r", cursor );
        if ( begin == std::string_view::npos )
        {
            break;
        }
        std::size_t end = view.find_first_of( " \t\r", begin );
        if ( end == std::string_view::npos )
        {
            end = view.size();
        }
        tokens.push_back( view.substr( begin, end - begin ) );
        cursor = end;
    }
    return tokens;
}

void PrintUsage()
{
    std::printf(
        "commands:\n"
        "  load <character_id>        load the character and bind this connection to it\n"
        "  inv                        re-read the bound character and print its items\n"
        "  equip <item_uid> <slot>    equip an item into slot 1 (weapon) / 2 (armor) / 3\n"
        "  rank [count]               top-N by exp, straight out of the cache\n"
        "  quit                       close the connection and exit\n" );
}

// 요청 1개를 보내고 왕복이 끝날 때까지 이 스레드를 블로킹한다
// 대기는 REPL 스레드에서만 일어난다
// Close() 도 완료를 발화하므로 연결이 끊기면 대기도 끝난다
[[nodiscard]] bool Await( const std::function< void( atlas_console::Completion ) >& issue )
{
    auto answered = std::make_shared< std::promise< void > >();
    std::future< void > waiter = answered->get_future();
    issue( [answered] { answered->set_value(); } );
    if ( waiter.wait_for( kRequestTimeout ) == std::future_status::timeout )
    {
        std::printf( "no answer within %us - giving up on this connection\n",
                     static_cast< atlas::UInt32 >( kRequestTimeout.count() ) );
        return false;
    }
    return true;
}

// false = 세션 지속 불가. 호출자가 루프를 빠져나감
[[nodiscard]] bool RunCommand( const std::shared_ptr< atlas_console::ConsoleClient >& client,
                               const std::vector< std::string_view >& tokens )
{
    const std::string_view command = tokens.front();

    if ( command == "load" )
    {
        if ( tokens.size() != 2 )
        {
            std::printf( "usage: load <character_id>\n" );
            return true;
        }
        const std::optional< UInt64 > character_id = ParseUnsigned( tokens[1] );
        if ( !character_id.has_value() )
        {
            std::printf( "'%s' is not a whole number\n", std::string( tokens[1] ).c_str() );
            return true;
        }
        return Await( [&client, character_id]( atlas_console::Completion done )
                      { client->RequestLoad( *character_id, std::move( done ) ); } );
    }

    if ( command == "inv" )
    {
        return Await( [&client]( atlas_console::Completion done )
                      { client->RequestInventory( std::move( done ) ); } );
    }

    if ( command == "equip" )
    {
        if ( tokens.size() != 3 )
        {
            std::printf( "usage: equip <item_uid> <slot>\n" );
            return true;
        }
        const std::optional< UInt64 > item_uid = ParseUnsigned( tokens[1] );
        const std::optional< UInt64 > slot = ParseUnsigned( tokens[2] );
        if ( !item_uid.has_value() || !slot.has_value() || *slot > 0xFFU )
        {
            std::printf( "usage: equip <item_uid> <slot>, both whole numbers\n" );
            return true;
        }
        // [AD 8.2] 범위 밖/불일치 슬롯을 여기서 거르지 않는다
        // 서버 권위가 요점이다. 클라이언트가 미리 검증하면 그 검사가 가려진다
        return Await(
            [&client, item_uid, slot]( atlas_console::Completion done )
            {
                client->RequestEquip( *item_uid, static_cast< UInt8 >( *slot ), std::move( done ) );
            } );
    }

    if ( command == "rank" )
    {
        UInt16 count = kDefaultRankCount;
        if ( tokens.size() == 2 )
        {
            const std::optional< UInt64 > requested = ParseUnsigned( tokens[1] );
            if ( !requested.has_value() || *requested > 0xFFFFU )
            {
                std::printf( "usage: rank [count]\n" );
                return true;
            }
            count = static_cast< UInt16 >( *requested );
        }
        return Await( [&client, count]( atlas_console::Completion done )
                      { client->RequestRanking( count, std::move( done ) ); } );
    }

    std::printf( "unknown command '%s'\n", std::string( command ).c_str() );
    PrintUsage();
    return true;
}

}  // namespace

int main( int argc, char** argv )  // NOLINT - main 시그니처는 표준이 정함
{
    try
    {
        // 콘솔만, Warn 이상. 데모 머신에 로그 파일 쓰는 주체를 하나 더 늘리지 않음
        atlas::LogConfig log_config;
        log_config.level = atlas::LogLevel::Warn;
        atlas::LogInit( log_config );

        // [AD 5.4] host/port 는 비밀이 아니므로 .env 가 아닌 인자.
        // 박아 두면 스택이 옮겨간 순간 쓸 수 없게 됨
        std::string host = "127.0.0.1";
        UInt16 port = 7777;
        for ( std::size_t index = 1; index + 1 < static_cast< std::size_t >( argc ); index += 2 )
        {
            const std::string_view key( argv[index] );
            const std::string_view value( argv[index + 1] );
            if ( key == "--host" )
            {
                host = std::string( value );
            }
            else if ( key == "--port" )
            {
                const std::optional< UInt64 > parsed = ParseUnsigned( value );
                ATLAS_CHECK( parsed.has_value() && *parsed <= 0xFFFFU, "'{}' is not a port",
                             value );
                port = static_cast< UInt16 >( *parsed );
            }
            else
            {
                ATLAS_THROW( atlas::Exception, "unknown option '{}'", key );
            }
        }

        // 워커 1개. 연결이 1개뿐이라 풀은 검증하지 않는 동시성 주장이 됨
        atlas::IoRunner runner( 1 );
        const atlas::Endpoint endpoint( atlas::asio::ip::make_address( host ), port );
        auto client =
            std::make_shared< atlas_console::ConsoleClient >( runner.Context(), endpoint );
        runner.Start();

        if ( !Await( [&client]( atlas_console::Completion done )
                     { client->Connect( std::move( done ) ); } ) ||
             !client->Alive() )
        {
            client->Shutdown();
            runner.Stop();
            atlas::LogShutdown();
            return 69;
        }

        // [AD 9] getline 은 블로킹이다
        // I/O 스레드에서 하면 그 스레드의 모든 세션이 멈춘다
        // 터미널 대기는 이 스레드 전용
        PrintUsage();
        std::string line;
        while ( client->Alive() )
        {
            std::printf( "atlas> " );
            std::fflush( stdout );
            // EOF 는 에러가 아니라 종료. 아니면 닫힌 stdin 위에서 REPL 이 공회전
            if ( !std::getline( std::cin, line ) )
            {
                break;
            }
            const std::vector< std::string_view > tokens = Split( line );
            if ( tokens.empty() )
            {
                continue;
            }
            if ( tokens.front() == "quit" )
            {
                break;
            }
            if ( !RunCommand( client, tokens ) )
            {
                break;
            }
        }

        // [AD 9] 종료 순서. 연결을 먼저 닫아 남은 읽기를 완료시킨 뒤 큐를 비움
        client->Shutdown();
        runner.Stop();
        std::printf( "bye\n" );
        atlas::LogShutdown();
        return 0;
    }
    catch ( const std::exception& failure )
    {
        std::printf( "console client failed: %s\n", failure.what() );
        atlas::LogShutdown();
        return 70;
    }
}
