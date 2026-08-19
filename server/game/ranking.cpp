// =============================================================================
// exp 랭킹의 Redis 명령 구성과 응답 파싱
// =============================================================================

#include "game/ranking.h"

#include <charconv>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"
#include "atlas/core/types.h"
#include "atlas/redis/connection.h"

namespace atlas_demo
{
namespace
{

using atlas::UInt16;
using atlas::UInt64;

// ZSET score 는 double 이라 2^53 초과 exp 는 정확히 왕복하지 않음
// 이 축소를 감수하는 근거는 지급 기준이 사본이 아니라 DB 라는 것뿐
UInt64 ParseScore( std::string_view text )
{
    double score = 0.0;
    const std::from_chars_result parsed =
        std::from_chars( text.data(), text.data() + text.size(), score );
    if ( parsed.ec != std::errc{} || score <= 0.0 )
    {
        return 0;
    }
    return static_cast< UInt64 >( score );
}

UInt64 ParseMember( std::string_view text )
{
    UInt64 value = 0;
    const std::from_chars_result parsed =
        std::from_chars( text.data(), text.data() + text.size(), value );
    if ( parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() )
    {
        return 0;
    }
    return value;
}

}  // namespace

std::string ExpRankingKey( atlas::UInt16 server_id )
{
    return std::format( "ranking:{}:exp", server_id );
}

void ExpRanking::Record( atlas::Ctx ctx, atlas::UInt16 server_id, atlas::CharacterId character_id,
                         atlas::UInt64 exp )
{
    atlas::RedisCommand command{ .verb = "ZADD",
                                 .args = { ExpRankingKey( server_id ), std::to_string( exp ),
                                           std::to_string( atlas::IdValue( character_id ) ) } };
    runner_->Submit( ctx, command,
                     []( const atlas::Ctx&, const atlas::RedisResult& result )
                     {
                         if ( !result.ok )
                         {
                             // DEBUG. 캐시가 죽으면 로드마다 찍힘. 사본 유실은 사고가 아님
                             ATLAS_LOG_DEBUG( "ranking write skipped: redis did not answer" );
                         }
                     } );
}

void ExpRanking::Top( atlas::Ctx ctx, atlas::UInt16 server_id, atlas::UInt16 count,
                      TopHandler handler, atlas::RedisRunner::CompletionPoster poster )
{
    const UInt16 clamped = count > kMaxRankEntries ? kMaxRankEntries : count;
    if ( clamped == 0 )
    {
        handler( true, std::vector< RankEntry >{} );
        return;
    }

    // ZREVRANGE 의 stop 인덱스는 포함이라 마지막은 count - 1
    // [CS 4.1] clamped - 1 은 int 로 승격되므로 명시적으로 넓힘. clamped >= 1
    const atlas::UInt32 last_index = static_cast< atlas::UInt32 >( clamped ) - 1U;
    atlas::RedisCommand command{
        .verb = "ZREVRANGE",
        .args = { ExpRankingKey( server_id ), "0", std::to_string( last_index ), "WITHSCORES" } };

    runner_->Submit(
        ctx, command,
        [handler = std::move( handler )]( const atlas::Ctx&, const atlas::RedisResult& result )
        {
            std::vector< RankEntry > entries;
            if ( !result.ok )
            {
                handler( false, entries );
                return;
            }
            // WITHSCORES 는 member, score 가 번갈아 오는 평평한 배열
            // 짝 없는 마지막은 응답이 잘린 것이라 추측하지 않고 버림
            entries.reserve( result.values.size() / 2U );
            for ( std::size_t index = 0; index + 1U < result.values.size(); index += 2U )
            {
                const std::optional< std::string >& member = result.values[index];
                const std::optional< std::string >& score = result.values[index + 1U];
                if ( !member.has_value() || !score.has_value() )
                {
                    continue;
                }
                entries.push_back( RankEntry{
                    .character_id = static_cast< atlas::CharacterId >( ParseMember( *member ) ),
                    .exp = ParseScore( *score ) } );
            }
            handler( true, entries );
        },
        std::move( poster ) );
}

}  // namespace atlas_demo
