// =============================================================================
// 콘솔 클라이언트 구현. 요청 1개씩 왕복하며 응답을 그대로 출력
// =============================================================================

#include "console_client/console_client.h"

#include <cstdio>
#include <span>
#include <string>
#include <tuple>
#include <utility>

#include "atlas/core/ctx.h"
#include "atlas/core/error.h"
#include "atlas/core/types.h"
#include "atlas/proto/frame.h"
#include "game/equip_service.h"
#include "game/handlers.h"
#include "game/inventory.h"
#include "generated/info/item_info.h"
#include "generated/pkt/pkt_codec.h"

namespace atlas_console
{

namespace
{

using atlas::Byte;
using atlas::Int32;
using atlas::UInt16;
using atlas::UInt32;
using atlas::UInt64;
using atlas::UInt8;

[[nodiscard]] unsigned long long Wide( UInt64 value )
{
    return static_cast< unsigned long long >( value );
}

[[nodiscard]] const char* DescribeLoadResult( UInt8 code )
{
    switch ( static_cast< atlas_demo::LoadResult >( code ) )
    {
        case atlas_demo::LoadResult::Ok:
            return "ok";
        case atlas_demo::LoadResult::NotFound:
            return "no such character";
        case atlas_demo::LoadResult::Unavailable:
            return "unavailable (the database pool did not lease a connection)";
        default:
            return "unknown";
    }
}

// 핸들러가 서비스 호출 전에 내리는 두 거부는 EquipResult 열거자가 아님
[[nodiscard]] std::string DescribeEquipCode( UInt8 code )
{
    if ( code == atlas_demo::kEquipResponseNotLoaded )
    {
        return "no character is loaded on this connection";
    }
    if ( code == atlas_demo::kEquipResponseUnavailable )
    {
        return "unavailable (the database pool did not lease a connection)";
    }
    return std::string(
        atlas_demo::DescribeEquipResult( static_cast< atlas_demo::EquipResult >( code ) ) );
}

// [AD 8.2] item.csv 가 허용하는 슬롯. 이 값이 있어야 SlotMismatch 거부가 읽힘
[[nodiscard]] UInt8 StaticSlotOf( UInt32 item_id )
{
    const atlas::generated::ItemInfoRow* info = atlas::generated::FindItemInfo( item_id );
    return info == nullptr ? UInt8{ 0 } : info->slot_;
}

void PrintItemLine( UInt64 item_uid, UInt32 item_id, UInt16 stack_count, UInt8 slot )
{
    const std::string worn(
        atlas_demo::DescribeSlot( static_cast< atlas_demo::EquipSlot >( slot ) ) );
    std::printf( "  uid=%llu item_id=%u stack=%u slot=%u(%s) csv_slot=%u\n", Wide( item_uid ),
                 item_id, stack_count, slot, worn.c_str(), StaticSlotOf( item_id ) );
}

}  // namespace

ConsoleClient::ConsoleClient( atlas::IoContext& io_context, atlas::Endpoint endpoint )
    : strand_( atlas::asio::make_strand( io_context ) ),
      socket_( strand_ ),
      endpoint_( std::move( endpoint ) )
{
}

// =============================================================================
// 진입점 - 아무 스레드에서 호출 가능
// =============================================================================

void ConsoleClient::Connect( Completion done )
{
    auto self = shared_from_this();
    atlas::asio::post(
        strand_, atlas::Guarded(
                     atlas::Ctx{},
                     [self, done = std::move( done )]() mutable
                     {
                         self->BeginRequest( std::move( done ) );
                         self->socket_.async_connect(
                             self->endpoint_,
                             atlas::asio::bind_executor(
                                 self->strand_,
                                 atlas::Guarded( atlas::Ctx{}, [self]( const atlas::ErrorCode& err )
                                                 { self->OnConnect( err ); } ) ) );
                     } ) );
}

void ConsoleClient::RequestLoad( UInt64 character_id, Completion done )
{
    auto self = shared_from_this();
    atlas::asio::post(
        strand_, atlas::Guarded( atlas::Ctx{},
                                 [self, character_id, done = std::move( done )]() mutable
                                 {
                                     self->BeginRequest( std::move( done ) );
                                     if ( self->closed_.load() )
                                     {
                                         std::printf( "not connected\n" );
                                         self->FinishRequest();
                                         return;
                                     }
                                     self->items_only_ = false;
                                     self->awaited_opcode_ = atlas_demo::kOpCharacterLoadResponse;
                                     self->payload_.clear();
                                     atlas::generated::WriteLe( self->payload_, character_id );
                                     self->SendFrame( atlas_demo::kOpCharacterLoadRequest );
                                 } ) );
}

void ConsoleClient::RequestInventory( Completion done )
{
    auto self = shared_from_this();
    atlas::asio::post(
        strand_, atlas::Guarded( atlas::Ctx{},
                                 [self, done = std::move( done )]() mutable
                                 {
                                     self->BeginRequest( std::move( done ) );
                                     if ( self->closed_.load() )
                                     {
                                         std::printf( "not connected\n" );
                                         self->FinishRequest();
                                         return;
                                     }
                                     if ( !self->loaded_ )
                                     {
                                         std::printf( "load <character_id> first\n" );
                                         self->FinishRequest();
                                         return;
                                     }
                                     self->items_only_ = true;
                                     self->awaited_opcode_ = atlas_demo::kOpCharacterLoadResponse;
                                     self->payload_.clear();
                                     atlas::generated::WriteLe( self->payload_,
                                                                self->character_id_ );
                                     self->SendFrame( atlas_demo::kOpCharacterLoadRequest );
                                 } ) );
}

void ConsoleClient::RequestEquip( UInt64 item_uid, UInt8 slot, Completion done )
{
    auto self = shared_from_this();
    atlas::asio::post( strand_,
                       atlas::Guarded( atlas::Ctx{},
                                       [self, item_uid, slot, done = std::move( done )]() mutable
                                       {
                                           self->BeginRequest( std::move( done ) );
                                           if ( self->closed_.load() )
                                           {
                                               std::printf( "not connected\n" );
                                               self->FinishRequest();
                                               return;
                                           }
                                           self->awaited_opcode_ = atlas_demo::kOpEquipResponse;
                                           self->payload_.clear();
                                           atlas::generated::WriteLe( self->payload_, item_uid );
                                           atlas::generated::WriteLe( self->payload_, slot );
                                           self->SendFrame( atlas_demo::kOpEquipRequest );
                                       } ) );
}

void ConsoleClient::RequestRanking( UInt16 count, Completion done )
{
    auto self = shared_from_this();
    atlas::asio::post( strand_,
                       atlas::Guarded( atlas::Ctx{},
                                       [self, count, done = std::move( done )]() mutable
                                       {
                                           self->BeginRequest( std::move( done ) );
                                           if ( self->closed_.load() )
                                           {
                                               std::printf( "not connected\n" );
                                               self->FinishRequest();
                                               return;
                                           }
                                           self->awaited_opcode_ = atlas_demo::kOpRankingResponse;
                                           self->payload_.clear();
                                           atlas::generated::WriteLe( self->payload_, count );
                                           self->SendFrame( atlas_demo::kOpRankingRequest );
                                       } ) );
}

void ConsoleClient::Shutdown()
{
    auto self = shared_from_this();
    atlas::asio::post( strand_, atlas::Guarded( atlas::Ctx{}, [self] { self->Close(); } ) );
}

// =============================================================================
// strand 위에서만 실행되는 처리
// =============================================================================

void ConsoleClient::OnConnect( const atlas::ErrorCode& error )
{
    if ( error )
    {
        std::printf( "connect failed: %s\n", error.message().c_str() );
        Close();
        return;
    }
    std::printf( "connected to %s:%u\n", endpoint_.address().to_string().c_str(),
                 endpoint_.port() );
    ReadMore();
    FinishRequest();
}

void ConsoleClient::ReadMore()
{
    auto self = shared_from_this();
    socket_.async_read_some(
        atlas::asio::buffer( read_buffer_ ),
        atlas::asio::bind_executor(
            strand_, atlas::Guarded( atlas::Ctx{},
                                     [self]( const atlas::ErrorCode& error, std::size_t bytes_read )
                                     { self->OnRead( error, bytes_read ); } ) ) );
}

void ConsoleClient::OnRead( const atlas::ErrorCode& error, std::size_t bytes_read )
{
    if ( closed_.load() )
    {
        return;
    }
    if ( error )
    {
        std::printf( "connection lost: %s\n", error.message().c_str() );
        OnTransportError();
        return;
    }

    frames_.clear();
    const atlas::FrameError framing =
        reader_.Feed( std::span< const Byte >( read_buffer_.data(), bytes_read ), frames_ );
    for ( const atlas::Frame& frame : frames_ )
    {
        OnFrame( frame );
    }
    if ( framing != atlas::FrameError::None )
    {
        // [AD 8.1] 신뢰할 수 없게 된 스트림. 서버와 같은 판정으로 읽기 중단
        std::printf( "framing error - the stream is no longer trustworthy\n" );
        OnTransportError();
        return;
    }
    if ( closed_.load() )
    {
        return;
    }
    ReadMore();
}

void ConsoleClient::OnFrame( const atlas::Frame& frame )
{
    if ( closed_.load() )
    {
        return;
    }
    if ( frame.opcode != awaited_opcode_ )
    {
        std::printf( "unexpected opcode 0x%04x - closing\n", frame.opcode );
        OnTransportError();
        return;
    }

    if ( frame.opcode == atlas_demo::kOpCharacterLoadResponse )
    {
        PrintLoadResponse( frame );
    }
    else if ( frame.opcode == atlas_demo::kOpEquipResponse )
    {
        PrintEquipResponse( frame );
    }
    else
    {
        PrintRankingResponse( frame );
    }

    awaited_opcode_ = 0;
    FinishRequest();
}

void ConsoleClient::PrintLoadResponse( const atlas::Frame& frame )
{
    std::span< const Byte > cursor( frame.payload );
    UInt8 result = 0;
    if ( !atlas::generated::ReadLe( cursor, result ) )
    {
        std::printf( "load: truncated response\n" );
        OnTransportError();
        return;
    }
    if ( result != static_cast< UInt8 >( atlas_demo::LoadResult::Ok ) )
    {
        std::printf( "load refused: %s\n", DescribeLoadResult( result ) );
        return;
    }

    UInt64 character_id = 0;
    UInt64 account_uid = 0;
    std::string name;
    Int32 pos_x = 0;
    Int32 pos_y = 0;
    UInt16 level = 0;
    UInt64 exp = 0;
    UInt16 item_count = 0;
    const bool decoded =
        atlas::generated::ReadLe( cursor, character_id ) &&
        atlas::generated::ReadLe( cursor, account_uid ) &&
        atlas::generated::ReadUtf8( cursor, name ) && atlas::generated::ReadLe( cursor, pos_x ) &&
        atlas::generated::ReadLe( cursor, pos_y ) && atlas::generated::ReadLe( cursor, level ) &&
        atlas::generated::ReadLe( cursor, exp ) && atlas::generated::ReadLe( cursor, item_count );
    if ( !decoded )
    {
        std::printf( "load: truncated response\n" );
        OnTransportError();
        return;
    }

    character_id_ = character_id;
    loaded_ = true;

    if ( !items_only_ )
    {
        std::printf( "character %llu account=%llu name=%s level=%u exp=%llu pos=(%d,%d)\n",
                     Wide( character_id ), Wide( account_uid ), name.c_str(), level, Wide( exp ),
                     pos_x, pos_y );
    }
    std::printf( "inventory (%u items)\n", item_count );
    for ( UInt16 index = 0; index < item_count; ++index )
    {
        UInt64 item_uid = 0;
        UInt32 item_id = 0;
        UInt16 stack_count = 0;
        UInt8 slot = 0;
        if ( !atlas::generated::ReadLe( cursor, item_uid ) ||
             !atlas::generated::ReadLe( cursor, item_id ) ||
             !atlas::generated::ReadLe( cursor, stack_count ) ||
             !atlas::generated::ReadLe( cursor, slot ) )
        {
            std::printf( "load: the item list is truncated\n" );
            OnTransportError();
            return;
        }
        PrintItemLine( item_uid, item_id, stack_count, slot );
    }
}

void ConsoleClient::PrintEquipResponse( const atlas::Frame& frame )
{
    std::span< const Byte > cursor( frame.payload );
    UInt8 code = 0;
    UInt64 item_uid = 0;
    UInt8 slot = 0;
    UInt64 unequipped = 0;
    if ( !atlas::generated::ReadLe( cursor, code ) ||
         !atlas::generated::ReadLe( cursor, item_uid ) ||
         !atlas::generated::ReadLe( cursor, slot ) ||
         !atlas::generated::ReadLe( cursor, unequipped ) )
    {
        std::printf( "equip: truncated response\n" );
        OnTransportError();
        return;
    }

    if ( code == static_cast< UInt8 >( atlas_demo::EquipResult::Ok ) )
    {
        std::printf( "equip ok: uid=%llu -> slot %u", Wide( item_uid ), slot );
        if ( unequipped != 0 )
        {
            std::printf( " (uid=%llu was unequipped)", Wide( unequipped ) );
        }
        std::printf( "\n" );
        return;
    }

    // [AD 8.2] 거부됐지만 연결은 유지된다
    // 프레임도 체크섬도 정상이고 요청 내용만 틀린 경우다
    // 서버가 끊지 않고 답하므로 다음 명령도 응답받는다
    std::printf( "equip refused: %s (uid=%llu, slot %u) - connection stays up\n",
                 DescribeEquipCode( code ).c_str(), Wide( item_uid ), slot );
}

void ConsoleClient::PrintRankingResponse( const atlas::Frame& frame )
{
    std::span< const Byte > cursor( frame.payload );
    UInt8 result = 0;
    if ( !atlas::generated::ReadLe( cursor, result ) )
    {
        std::printf( "rank: truncated response\n" );
        OnTransportError();
        return;
    }
    if ( result != static_cast< UInt8 >( atlas_demo::RankingResult::Ok ) )
    {
        // 캐시 무응답. 빈 순위표를 찍으면 장애를 사실로 보고하게 됨
        std::printf( "rank unavailable: the cache did not answer\n" );
        return;
    }

    UInt16 count = 0;
    if ( !atlas::generated::ReadLe( cursor, count ) )
    {
        std::printf( "rank: truncated response\n" );
        OnTransportError();
        return;
    }
    std::printf( "ranking (%u entries, highest exp first)\n", count );
    for ( UInt16 index = 0; index < count; ++index )
    {
        UInt64 character_id = 0;
        UInt64 exp = 0;
        if ( !atlas::generated::ReadLe( cursor, character_id ) ||
             !atlas::generated::ReadLe( cursor, exp ) )
        {
            std::printf( "rank: the entry list is truncated\n" );
            OnTransportError();
            return;
        }
        std::printf( "  #%u character=%llu exp=%llu\n", index + 1U, Wide( character_id ),
                     Wide( exp ) );
    }
}

void ConsoleClient::BeginRequest( Completion done )
{
    if ( pending_done_ )
    {
        // 옛 완료를 발화해야 REPL 스레드가 영원히 대기하지 않음
        Completion abandoned = std::move( pending_done_ );
        pending_done_ = nullptr;
        abandoned();
    }
    pending_done_ = std::move( done );
}

void ConsoleClient::FinishRequest()
{
    if ( !pending_done_ )
    {
        return;
    }
    Completion done = std::move( pending_done_ );
    pending_done_ = nullptr;
    done();
}

void ConsoleClient::SendFrame( UInt16 opcode )
{
    ++send_seq_;
    wire_.clear();
    if ( !atlas::EncodeFrame( wire_, opcode, send_seq_, payload_ ) )
    {
        std::printf( "the payload does not fit a frame ([AD 8.1] caps it at 16 KiB)\n" );
        OnTransportError();
        return;
    }

    // 쓰기 동안 wire_ 를 붙들어도 안전 - 요청은 항상 1개만 in-flight
    auto self = shared_from_this();
    atlas::asio::async_write(
        socket_, atlas::asio::buffer( wire_ ),
        atlas::asio::bind_executor(
            strand_, atlas::Guarded( atlas::Ctx{},
                                     [self]( const atlas::ErrorCode& error, std::size_t )
                                     {
                                         if ( error && !self->closed_.load() )
                                         {
                                             std::printf( "send failed: %s\n",
                                                          error.message().c_str() );
                                             self->OnTransportError();
                                         }
                                     } ) ) );
}

void ConsoleClient::OnTransportError()
{
    if ( closed_.load() )
    {
        return;
    }
    Close();
}

void ConsoleClient::Close()
{
    if ( closed_.load() )
    {
        return;
    }
    closed_.store( true );

    atlas::ErrorCode ignored;
    std::ignore = socket_.shutdown( atlas::Socket::shutdown_both, ignored );
    std::ignore = socket_.close( ignored );

    // 맨 마지막에 무조건 실행한다
    // REPL 스레드는 이 완료에 막혀 있다
    // 발화하지 않는 Close() 는 세션 종료가 아니라 터미널 멈춤이 된다
    FinishRequest();
}

}  // namespace atlas_console
