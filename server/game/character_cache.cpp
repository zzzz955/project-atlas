// =============================================================================
// 캐시 값의 인코딩 - 디코딩과 GET / SET / DEL 명령
// =============================================================================

#include "game/character_cache.h"

#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/redis/connection.h"
#include "generated/pkt/pkt_codec.h"

namespace atlas_demo
{
namespace
{

using atlas::Byte;
using atlas::Int32;
using atlas::Int64;
using atlas::SysTime;
using atlas::UInt16;
using atlas::UInt32;
using atlas::UInt64;
using atlas::UInt8;

// DATETIME 에 소수부가 없어 초 단위는 무손실. character.cpp 의 절단과 같음
Int64 ToEpochSeconds( SysTime value )
{
    return std::chrono::duration_cast< std::chrono::seconds >( value.time_since_epoch() ).count();
}

SysTime FromEpochSeconds( Int64 seconds )
{
    return SysTime{
        std::chrono::duration_cast< SysTime::duration >( std::chrono::seconds{ seconds } ) };
}

// std::string 은 char 기반이고 Byte 는 아님
// 앨리어싱 여지를 남기지 않으려 reinterpret_cast 대신 명시 변환
std::string BytesToString( const std::vector< Byte >& bytes )
{
    std::string text;
    text.reserve( bytes.size() );
    for ( const Byte value : bytes )
    {
        text.push_back( static_cast< char >( std::to_integer< UInt8 >( value ) ) );
    }
    return text;
}

std::vector< Byte > StringToBytes( std::string_view text )
{
    std::vector< Byte > bytes;
    bytes.reserve( text.size() );
    for ( const char value : text )
    {
        bytes.push_back( static_cast< Byte >( static_cast< UInt8 >( value ) ) );
    }
    return bytes;
}

}  // namespace

std::string CharacterCacheKey( atlas::UInt16 server_id, atlas::CharacterId character_id )
{
    return std::format( "character:{}:{}", server_id, atlas::IdValue( character_id ) );
}

std::string EncodeCachedCharacter( const CachedCharacter& value )
{
    std::vector< Byte > out;
    const atlas::generated::CharactersRow& row = value.character;
    atlas::generated::WriteLe( out, row.server_id_ );
    atlas::generated::WriteLe( out, atlas::IdValue( row.character_id_ ) );
    atlas::generated::WriteLe( out, atlas::IdValue( row.account_uid_ ) );
    atlas::generated::WriteUtf8( out, row.name_ );
    atlas::generated::WriteLe( out, row.pos_x_ );
    atlas::generated::WriteLe( out, row.pos_y_ );
    atlas::generated::WriteLe( out, row.level_ );
    atlas::generated::WriteLe( out, row.exp_ );
    atlas::generated::WriteLe( out, ToEpochSeconds( row.created_at_ ) );
    // 존재 플래그 유지. 캐시된 NULL 과 캐시된 epoch 0 은 다른 사실
    atlas::generated::WriteBool( out, row.last_login_at_.has_value() );
    atlas::generated::WriteLe(
        out, row.last_login_at_.has_value() ? ToEpochSeconds( *row.last_login_at_ ) : Int64{ 0 } );

    const UInt16 count = atlas::generated::WriteLength( out, value.items.size() );
    for ( std::size_t index = 0; index < static_cast< std::size_t >( count ); ++index )
    {
        const Item& entry = value.items[index];
        atlas::generated::WriteLe( out, entry.item_uid );
        atlas::generated::WriteLe( out, entry.item_id );
        atlas::generated::WriteLe( out, entry.stack_count );
        atlas::generated::WriteLe( out, static_cast< UInt8 >( entry.slot ) );
    }
    return BytesToString( out );
}

std::optional< CachedCharacter > DecodeCachedCharacter( std::string_view encoded )
{
    const std::vector< Byte > bytes = StringToBytes( encoded );
    std::span< const Byte > cursor( bytes );

    CachedCharacter value;
    atlas::generated::CharactersRow& row = value.character;
    UInt64 character_id = 0;
    UInt64 account_uid = 0;
    Int64 created_at = 0;
    bool has_last_login = false;
    Int64 last_login_at = 0;

    if ( !atlas::generated::ReadLe( cursor, row.server_id_ ) ||
         !atlas::generated::ReadLe( cursor, character_id ) ||
         !atlas::generated::ReadLe( cursor, account_uid ) ||
         !atlas::generated::ReadUtf8( cursor, row.name_ ) ||
         !atlas::generated::ReadLe( cursor, row.pos_x_ ) ||
         !atlas::generated::ReadLe( cursor, row.pos_y_ ) ||
         !atlas::generated::ReadLe( cursor, row.level_ ) ||
         !atlas::generated::ReadLe( cursor, row.exp_ ) ||
         !atlas::generated::ReadLe( cursor, created_at ) ||
         !atlas::generated::ReadBool( cursor, has_last_login ) ||
         !atlas::generated::ReadLe( cursor, last_login_at ) )
    {
        return std::nullopt;
    }
    row.character_id_ = static_cast< atlas::CharacterId >( character_id );
    row.account_uid_ = static_cast< atlas::AccountId >( account_uid );
    row.created_at_ = FromEpochSeconds( created_at );
    row.last_login_at_ = has_last_login
                             ? std::optional< SysTime >{ FromEpochSeconds( last_login_at ) }
                             : std::nullopt;

    UInt16 count = 0;
    if ( !atlas::generated::ReadLe( cursor, count ) )
    {
        return std::nullopt;
    }
    value.items.reserve( static_cast< std::size_t >( count ) );
    for ( std::size_t index = 0; index < static_cast< std::size_t >( count ); ++index )
    {
        Item entry;
        UInt8 slot = 0;
        if ( !atlas::generated::ReadLe( cursor, entry.item_uid ) ||
             !atlas::generated::ReadLe( cursor, entry.item_id ) ||
             !atlas::generated::ReadLe( cursor, entry.stack_count ) ||
             !atlas::generated::ReadLe( cursor, slot ) )
        {
            return std::nullopt;
        }
        entry.slot = static_cast< EquipSlot >( slot );
        value.items.push_back( entry );
    }
    return value;
}

void CharacterCache::Get( atlas::Ctx ctx, atlas::UInt16 server_id, atlas::CharacterId character_id,
                          GetHandler handler, atlas::RedisRunner::CompletionPoster poster )
{
    atlas::RedisCommand command{ .verb = "GET",
                                 .args = { CharacterCacheKey( server_id, character_id ) } };
    runner_->Submit(
        ctx, command,
        [handler = std::move( handler )]( const atlas::Ctx&, const atlas::RedisResult& result )
        {
            // 연결 없음 - 키 없음 - 못 읽는 값. 세 실패를 일부러 한 답으로 접음
            if ( !result.ok || result.values.empty() || !result.values.front().has_value() )
            {
                handler( std::nullopt );
                return;
            }
            handler( DecodeCachedCharacter( *result.values.front() ) );
        },
        std::move( poster ) );
}

void CharacterCache::Put( atlas::Ctx ctx, atlas::UInt16 server_id, const CachedCharacter& value )
{
    atlas::RedisCommand command{
        .verb = "SET",
        .args = { CharacterCacheKey( server_id, value.character.character_id_ ),
                  EncodeCachedCharacter( value ), "EX", std::to_string( kTtlSeconds ) } };
    runner_->Submit( ctx, command,
                     []( const atlas::Ctx&, const atlas::RedisResult& result )
                     {
                         if ( !result.ok )
                         {
                             ATLAS_LOG_DEBUG( "cache fill skipped: redis did not answer" );
                         }
                     } );
}

void CharacterCache::Invalidate( atlas::Ctx ctx, atlas::UInt16 server_id,
                                 atlas::CharacterId character_id )
{
    // [AD 10.2] 캐시 미설정은 무효화할 것이 없다는 뜻이지 실패가 아님
    // [AD 16.1h] 없으면 미설정 호스트의 즉시 거부가 ok == false 로 옴
    // 성공한 장착마다 아래 경고가 찍힘
    // 실측 1098 요청에 1098 경고. 진짜 실패가 묻힘
    if ( !runner_->IsConfigured() )
    {
        return;
    }

    atlas::RedisCommand command{ .verb = "DEL",
                                 .args = { CharacterCacheKey( server_id, character_id ) } };
    runner_->Submit( ctx, command,
                     []( const atlas::Ctx&, const atlas::RedisResult& result )
                     {
                         if ( !result.ok )
                         {
                             // Put 과 달리 WARN
                             // 채우기 실패는 사본이 없는 것뿐
                             // 무효화 실패는 TTL 동안 틀린 사본을 남김
                             ATLAS_LOG_WARN(
                                 "cache invalidation failed: a stale character copy may "
                                 "be served until it expires" );
                         }
                     } );
}

}  // namespace atlas_demo
