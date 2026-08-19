// =============================================================================
// GAME 서버 구현: 페이로드 코덱 - 콜드 로드 - 디스패치 - 서버 수명
// =============================================================================

#include "game/handlers.h"

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/error.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/db/prepared_statement.h"
#include "atlas/db/transaction.h"
#include "atlas/net/net_types.h"
#include "atlas/net/session.h"
#include "atlas/proto/frame.h"
#include "atlas/proto/session_framing.h"
#include "atlas/redis/connection.h"
#include "game/character.h"
#include "game/character_cache.h"
#include "game/equip_service.h"
#include "game/inventory.h"
#include "game/ranking.h"
#include "generated/db/character_items_row.h"
#include "generated/db/characters_row.h"
#include "generated/pkt/pkt_codec.h"

namespace atlas_demo
{

namespace
{

using atlas::Byte;
using atlas::DbRow;
using atlas::DbValue;
using atlas::UInt16;
using atlas::UInt32;
using atlas::UInt64;
using atlas::UInt8;

// 캐시를 기다리는 기동 예산. 유한하고 치명적이지 않음(Start() 참고)
constexpr atlas::Seconds kRedisReadyBudget{ 2 };

// 웜 경로의 유일한 쓰기. DB 스레드에서 임대 커넥션으로 실행
// 단일 행 UPDATE 는 트랜잭션 없이도 원자적이라 스코프가 없음
void TouchLoginOnDbThread( atlas::Connection& connection, UInt16 server_id,
                           atlas::CharacterId character_id )
{
    const std::array< DbValue, 3 > parameters{ DbValue{ PersistableNow() },
                                               DbValue{ static_cast< UInt64 >( server_id ) },
                                               DbValue{ atlas::IdValue( character_id ) } };
    connection.Prepare( kTouchCharacterLoginSql ).Execute( parameters );
}

// =============================================================================
// 페이로드 코덱
// =============================================================================

// [AD 8.5] 리틀엔디언 원시 연산은 생성 codec 에서 옴
// 손으로 쓴 것은 이 두 메시지의 필드 순서뿐
// 아직 이를 선언하는 계약 소스가 없기 때문

std::vector< Byte > EncodeLoadResponse( const CharacterSnapshot& snapshot )
{
    std::vector< Byte > payload;
    atlas::generated::WriteLe( payload, static_cast< UInt8 >( snapshot.result ) );
    if ( snapshot.result != LoadResult::Ok )
    {
        return payload;
    }

    atlas::generated::WriteLe( payload, atlas::IdValue( snapshot.character.character_id_ ) );
    atlas::generated::WriteLe( payload, atlas::IdValue( snapshot.character.account_uid_ ) );
    atlas::generated::WriteUtf8( payload, snapshot.character.name_ );
    atlas::generated::WriteLe( payload, snapshot.character.pos_x_ );
    atlas::generated::WriteLe( payload, snapshot.character.pos_y_ );
    atlas::generated::WriteLe( payload, snapshot.character.level_ );
    atlas::generated::WriteLe( payload, snapshot.character.exp_ );

    // [AD 8.5] 반복 필드의 접두는 원소 수. 클램프는 생성 헬퍼의 것
    const UInt16 count = atlas::generated::WriteLength( payload, snapshot.items.size() );
    for ( std::size_t index = 0; index < static_cast< std::size_t >( count ); ++index )
    {
        const Item& item = snapshot.items[index];
        atlas::generated::WriteLe( payload, item.item_uid );
        atlas::generated::WriteLe( payload, item.item_id );
        atlas::generated::WriteLe( payload, item.stack_count );
        atlas::generated::WriteLe( payload, static_cast< UInt8 >( item.slot ) );
    }
    return payload;
}

std::vector< Byte > EncodeEquipResponse( UInt8 result, UInt64 item_uid, UInt8 slot,
                                         UInt64 unequipped_item_uid )
{
    std::vector< Byte > payload;
    atlas::generated::WriteLe( payload, result );
    atlas::generated::WriteLe( payload, item_uid );
    atlas::generated::WriteLe( payload, slot );
    atlas::generated::WriteLe( payload, unequipped_item_uid );
    return payload;
}

std::vector< Byte > EncodeRankingResponse( RankingResult result,
                                           const std::vector< RankEntry >& entries )
{
    std::vector< Byte > payload;
    atlas::generated::WriteLe( payload, static_cast< UInt8 >( result ) );
    if ( result != RankingResult::Ok )
    {
        return payload;
    }
    const UInt16 count = atlas::generated::WriteLength( payload, entries.size() );
    for ( std::size_t index = 0; index < static_cast< std::size_t >( count ); ++index )
    {
        atlas::generated::WriteLe( payload, atlas::IdValue( entries[index].character_id ) );
        atlas::generated::WriteLe( payload, entries[index].exp );
    }
    return payload;
}

}  // namespace

// =============================================================================
// 콜드 로드
// =============================================================================

// 로그인 시각을 캐릭터와 아이템을 읽는 같은 트랜잭션 안에서 씀
// 그래야 "T 에 로그인"과 T 이전 캐릭터 상태가 나란히 보이지 않음
void LoadCharacterOnDbThread( atlas::Ctx& ctx, atlas::Connection& connection, UInt16 server_id,
                              atlas::CharacterId character_id, CharacterSnapshot& out,
                              ExpRanking* ranking, const LoadFaultInjector& before_commit )
{
    const std::array< DbValue, 2 > key{ DbValue{ static_cast< UInt64 >( server_id ) },
                                        DbValue{ atlas::IdValue( character_id ) } };

    atlas::Transaction transaction( connection, ctx );

    const std::vector< DbRow > character_rows =
        connection.Prepare( atlas::generated::kCharactersSelectByPkSql ).Query( key );
    if ( character_rows.empty() )
    {
        // [AD 10] 쓴 것이 없으므로 스코프를 푸는 것이 곧 롤백
        out.result = LoadResult::NotFound;
        return;
    }

    atlas::generated::CharactersRow character = CharactersRowFromDb( character_rows.front() );
    character.last_login_at_ = PersistableNow();
    connection.Prepare( atlas::generated::kCharactersUpdateByPkSql )
        .Execute( CharactersUpdateParameters( character ) );

    const std::vector< DbRow > item_rows =
        connection.Prepare( kSelectItemsByCharacterSql ).Query( key );
    out.items.clear();
    out.items.reserve( item_rows.size() );
    for ( const DbRow& row : item_rows )
    {
        out.items.push_back( ItemFromRow( CharacterItemsRowFromDb( row ) ) );
    }

    if ( before_commit )
    {
        before_commit();
    }
    transaction.Commit();

    // 이 줄 아래는 커밋이 반환했기 때문에만 도달함
    // [AD 10.2] 사본에 DB 가 아직 거부할 수 있는 값을 남기지 않기 위한 순서
    if ( ranking != nullptr )
    {
        ranking->Record( ctx, server_id, character.character_id_, character.exp_ );
    }

    out.character = std::move( character );
    out.result = LoadResult::Ok;
}

// =============================================================================
// HandlerTable
// =============================================================================

void HandlerTable::Register( atlas::UInt16 opcode, Handler handler )
{
    ATLAS_CHECK( static_cast< bool >( handler ),
                 "opcode 0x{:04x} was registered with an empty handler", opcode );
    const auto inserted = handlers_.emplace( opcode, std::move( handler ) );
    // 한 opcode 의 두 번째 등록은 정책이 아니라 프로그래밍 오류
    // 승자를 삽입 순서가 정하고 그것은 파일 이름만 바뀌어도 달라짐
    ATLAS_CHECK( inserted.second, "opcode 0x{:04x} is registered twice", opcode );
}

const HandlerTable::Handler* HandlerTable::Find( atlas::UInt16 opcode ) const noexcept
{
    const auto found = handlers_.find( opcode );
    return found == handlers_.end() ? nullptr : &found->second;
}

// =============================================================================
// GameServer
// =============================================================================

GameServer::GameServer( const Options& options, const atlas::DbConnectionConfig& db_config,
                        const atlas::RedisConnectionConfig& redis_config )
    : options_( options ),
      pool_( db_config, options.db_pool_size == 0 ? std::size_t{ 1 } : options.db_pool_size ),
      db_runner_( pool_, options.db_threads, atlas::Seconds{ 5 } ),
      io_runner_( options.io_workers ),
      redis_( io_runner_.Context(), redis_config ),
      redis_runner_( redis_ ),
      ranking_( redis_runner_ ),
      character_cache_( redis_runner_ ),
      redis_enabled_( !redis_config.host.empty() )
{
    acceptor_ = std::make_unique< atlas::SessionAcceptor >(
        io_runner_.Context(), options.endpoint,
        [this]( const std::shared_ptr< atlas::Session >& session ) { OnAccept( session ); } );
    RegisterHandlers();
}

GameServer::~GameServer() { Stop(); }

void GameServer::Start()
{
    if ( running_ )
    {
        return;
    }
    running_ = true;
    acceptor_->Start();
    db_runner_.Start();
    io_runner_.Start();
    if ( redis_enabled_ )
    {
        // io_runner_.Start() 뒤. 연결이 그 context 위에서 돎
        // [AD 10.2] 대기는 유한하고 치명적이지 않음. 사본이 기동 의존이 되면 안 됨
        redis_.Start();
        if ( !redis_.WaitUntilReady( kRedisReadyBudget ) )
        {
            ATLAS_LOG_WARN(
                "redis is configured but did not answer within {}s - the character cache and the "
                "ranking degrade to the database until it does",
                kRedisReadyBudget.count() );
        }
    }
    ATLAS_LOG_INFO( "GAME listening on {}:{} (server_id={}, io_workers={}, db_threads={})",
                    acceptor_->LocalEndpoint().address().to_string(),
                    acceptor_->LocalEndpoint().port(), options_.server_id, io_runner_.WorkerCount(),
                    db_runner_.ThreadCount() );
}

void GameServer::Stop() noexcept
{
    if ( !running_ )
    {
        return;
    }
    running_ = false;

    // 1. 앞문
    acceptor_->Stop();

    // 2. 살아 있는 세션. 락 안에서 모으고 밖에서 닫음
    //    인라인으로 도는 close 핸들러는 Forget() 에 재진입함
    std::vector< std::shared_ptr< atlas::Session > > live;
    {
        const std::scoped_lock guard( sessions_mutex_ );
        live.reserve( sessions_.size() );
        for ( const auto& entry : sessions_ )
        {
            if ( std::shared_ptr< atlas::Session > session = entry.second.lock() )
            {
                live.push_back( std::move( session ) );
            }
        }
        sessions_.clear();
    }
    for ( const std::shared_ptr< atlas::Session >& session : live )
    {
        session->Close();
    }

    // 3. DB 풀. 완료가 세션 strand 로 돌아오므로 I/O 풀보다 먼저 멈춤
    db_runner_.Stop();

    // 4. 캐시. 재접속 루프가 미결 작업이라 work guard 를 먼저 놓으면 큐가 안 빠짐
    redis_.Stop();

    // 5. I/O 풀
    io_runner_.Stop();
}

const atlas::Endpoint& GameServer::LocalEndpoint() const noexcept
{
    return acceptor_->LocalEndpoint();
}

std::size_t GameServer::LiveSessionCount() const
{
    const std::scoped_lock guard( sessions_mutex_ );
    return sessions_.size();
}

GameServer::Counters GameServer::ReadCounters() const
{
    // 일관 스냅숏이 아니고 그럴 필요도 없음
    // 다섯을 한꺼번에 얼리려면 큐 뮤텍스를 잡아야 함
    // 그러면 표본 주기마다 DB 스레드가 멈춤
    return Counters{ .live_sessions = LiveSessionCount(),
                     .db_queue_depth = db_runner_.PendingCount(),
                     .db_queue_capacity = db_runner_.QueueCapacity(),
                     .db_rejected = db_runner_.RejectedCount(),
                     .db_acquire_failures = db_runner_.AcquireFailureCount(),
                     .db_acquire_wait_micros = db_runner_.AcquireWaitMicros() };
}

void GameServer::RegisterHandlers()
{
    handlers_.Register(
        kOpCharacterLoadRequest,
        [this]( const std::shared_ptr< atlas::Session >& session,
                const std::shared_ptr< SessionState >& state, const atlas::Frame& frame )
        { HandleCharacterLoad( session, state, frame ); } );
    handlers_.Register( kOpEquipRequest, [this]( const std::shared_ptr< atlas::Session >& session,
                                                 const std::shared_ptr< SessionState >& state,
                                                 const atlas::Frame& frame )
                        { HandleEquip( session, state, frame ); } );
    handlers_.Register( kOpRankingRequest, [this]( const std::shared_ptr< atlas::Session >& session,
                                                   const std::shared_ptr< SessionState >& state,
                                                   const atlas::Frame& frame )
                        { HandleRanking( session, state, frame ); } );
}

void GameServer::OnAccept( const std::shared_ptr< atlas::Session >& session )
{
    // accept strand. Session::Create 뒤 Session::Start() 앞, 콜백 설치 창
    auto state = std::make_shared< SessionState >();
    state->server_id = options_.server_id;

    atlas::AttachFrameReader(
        session,
        [this, state]( const std::shared_ptr< atlas::Session >& source, const atlas::Frame& frame )
        {
            // [AD 11.2b] 이미 세션의 Guarded 읽기 핸들러 안
            // 여기서 throw 해도 I/O 스레드는 죽지 않음
            // 로그가 남고 이 연결만 닫힘
            Dispatch( source, state, frame );
        } );
    session->SetCloseHandler( [this]( const std::shared_ptr< atlas::Session >& closed )
                              { Forget( closed->Id() ); } );

    const std::scoped_lock guard( sessions_mutex_ );
    sessions_.emplace( atlas::IdValue( session->Id() ), session );
}

void GameServer::Forget( atlas::SessionId id )
{
    const std::scoped_lock guard( sessions_mutex_ );
    sessions_.erase( atlas::IdValue( id ) );
}

void GameServer::Dispatch( const std::shared_ptr< atlas::Session >& session,
                           const std::shared_ptr< SessionState >& state, const atlas::Frame& frame )
{
    const HandlerTable::Handler* handler = handlers_.Find( frame.opcode );
    if ( handler == nullptr )
    {
        // [AD 8.1] 무시가 아니라 종료
        // 모르는 opcode 는 부분 이해가 아니라 다른 프로토콜을 말하는 피어
        // 계속 읽는 것은 추측
        ATLAS_LOG_WARN( "session {} sent unknown opcode 0x{:04x} - closing",
                        atlas::IdValue( session->Id() ), frame.opcode );
        session->Close();
        return;
    }
    ( *handler )( session, state, frame );
}

atlas::Ctx GameServer::MakeCtx( const std::shared_ptr< atlas::Session >& session,
                                const std::shared_ptr< SessionState >& state )
{
    atlas::Ctx ctx;
    ctx.trace_id = next_trace_id_.fetch_add( 1 );
    ctx.session_id = session->Id();
    ctx.character_id = state->character_id;
    return ctx;
}

atlas::DbRunner::CompletionPoster GameServer::PosterFor(
    const std::shared_ptr< atlas::Session >& session )
{
    // 세션 참조가 아니라 strand 사본. poster 가 이 프레임보다 오래 삶
    atlas::Strand strand = session->GetStrand();
    return [strand]( std::function< void() > completion )
    { atlas::asio::post( strand, std::move( completion ) ); };
}

void GameServer::HandleCharacterLoad( const std::shared_ptr< atlas::Session >& session,
                                      const std::shared_ptr< SessionState >& state,
                                      const atlas::Frame& frame )
{
    std::span< const Byte > cursor( frame.payload );
    UInt64 requested = 0;
    if ( !atlas::generated::ReadLe( cursor, requested ) )
    {
        ATLAS_LOG_WARN( "session {} sent a truncated character load - closing",
                        atlas::IdValue( session->Id() ) );
        session->Close();
        return;
    }

    // [AD 12] 요청이 자기 캐릭터를 지정하는 구멍
    // 인증이 나중 슬라이스라 이 연결의 플레이 권한을 증명할 것이 없음
    // [AD 8.2] 로드 이후는 성립함. 세션 캐릭터는 DB 가 행을 확인한 뒤에만 정해짐
    atlas::Ctx ctx = MakeCtx( session, state );
    const auto character_id = static_cast< atlas::CharacterId >( requested );
    ctx.character_id = character_id;

    // 캐시를 먼저 묻지만 그 답은 DB 작업의 양만 정하고 작업을 대체하지 않음
    // 로그인은 쓰기이고 어떤 사본도 쓰기를 흡수할 수 없음
    character_cache_.Get(
        ctx, options_.server_id, character_id,
        [this, session, state, ctx, character_id]( std::optional< CachedCharacter > cached )
        { SubmitCharacterLoad( session, state, ctx, character_id, std::move( cached ) ); },
        PosterFor( session ) );
}

void GameServer::SubmitCharacterLoad( const std::shared_ptr< atlas::Session >& session,
                                      const std::shared_ptr< SessionState >& state,
                                      const atlas::Ctx& ctx, atlas::CharacterId character_id,
                                      std::optional< CachedCharacter > cached )
{
    auto snapshot = std::make_shared< CharacterSnapshot >();
    auto warm = std::make_shared< std::optional< CachedCharacter > >( std::move( cached ) );
    const bool was_warm = warm->has_value();
    const UInt16 server_id = options_.server_id;

    const atlas::SubmitResult submitted = db_runner_.Submit(
        ctx,
        [this, snapshot, warm, server_id, character_id]( atlas::Ctx& job_ctx,
                                                         atlas::Connection* connection )
        {
            if ( connection == nullptr )
            {
                snapshot->result = LoadResult::Unavailable;
                return;
            }
            if ( warm->has_value() )
            {
                // 웜 답은 사본이라 TTL 안에 지워진 캐릭터도 Ok 로 읽힘
                // TTL 이 상한이고 쓰기 경로가 이 게임이 만드는 변경을 무효화함
                TouchLoginOnDbThread( *connection, server_id, character_id );
                snapshot->character = ( *warm )->character;
                snapshot->items = ( *warm )->items;
                snapshot->result = LoadResult::Ok;
                return;
            }
            LoadCharacterOnDbThread( job_ctx, *connection, server_id, character_id, *snapshot,
                                     &ranking_ );
        },
        [this, session, state, snapshot, was_warm, server_id]( const atlas::Ctx& done_ctx )
        {
            // 세션 strand 위라 state 와 송신 카운터에 락이 필요 없음
            if ( snapshot->result == LoadResult::Ok )
            {
                state->loaded = true;
                state->character_id = snapshot->character.character_id_;
                if ( !was_warm )
                {
                    character_cache_.Put( done_ctx, server_id,
                                          CachedCharacter{ .character = snapshot->character,
                                                           .items = snapshot->items } );
                }
            }
            ++state->send_seq;
            atlas::SendFrame( *session, kOpCharacterLoadResponse, state->send_seq,
                              EncodeLoadResponse( *snapshot ) );
        },
        PosterFor( session ) );

    if ( submitted != atlas::SubmitResult::Accepted )
    {
        // 거부이지 유실이 아님
        // 작업도 완료도 없으므로 여기서 답하지 않으면 클라이언트가 영원히 기다림
        // 풀 고갈과 큐 만석은 같은 사실
        snapshot->result = LoadResult::Unavailable;
        ++state->send_seq;
        atlas::SendFrame( *session, kOpCharacterLoadResponse, state->send_seq,
                          EncodeLoadResponse( *snapshot ) );
    }
}

void GameServer::HandleEquip( const std::shared_ptr< atlas::Session >& session,
                              const std::shared_ptr< SessionState >& state,
                              const atlas::Frame& frame )
{
    if ( !state->loaded )
    {
        // [AD 8.2] 패킷의 캐릭터 id 를 믿지 않고 여기서 거부
        // 로드되지 않은 세션에는 행동 근거가 될 서버측 신원이 없음
        ++state->send_seq;
        atlas::SendFrame( *session, kOpEquipResponse, state->send_seq,
                          EncodeEquipResponse( kEquipResponseNotLoaded, 0, 0, 0 ) );
        return;
    }

    std::span< const Byte > cursor( frame.payload );
    UInt64 item_uid = 0;
    UInt8 raw_slot = 0;
    if ( !atlas::generated::ReadLe( cursor, item_uid ) ||
         !atlas::generated::ReadLe( cursor, raw_slot ) )
    {
        ATLAS_LOG_WARN( "session {} sent a truncated equip request - closing",
                        atlas::IdValue( session->Id() ) );
        session->Close();
        return;
    }

    const EquipService::Request request{ .server_id = options_.server_id,
                                         .character_id = state->character_id,
                                         .item_uid = item_uid,
                                         .slot = static_cast< EquipSlot >( raw_slot ) };

    // 비어 있으면 작업이 결과를 못 낸 것
    // 커넥션 미임대이거나 DB 스레드 가드가 예외를 삼킨 경우
    // 어느 쪽이든 기다리게 두지 않고 알림
    auto outcome = std::make_shared< std::optional< EquipService::Outcome > >();
    const atlas::SubmitResult submitted = db_runner_.Submit(
        MakeCtx( session, state ),
        [this, outcome, request]( atlas::Ctx& job_ctx, atlas::Connection* connection )
        {
            if ( connection == nullptr )
            {
                return;
            }
            *outcome = equip_service_.Equip( job_ctx, *connection, request );
        },
        [this, session, state, outcome, request]( const atlas::Ctx& done_ctx )
        {
            const UInt8 code = outcome->has_value() ? static_cast< UInt8 >( ( *outcome )->result )
                                                    : kEquipResponseUnavailable;
            const UInt64 unequipped = outcome->has_value() ? ( *outcome )->unequipped_item_uid : 0;
            // [AD 10.2] 갱신이 아니라 무효화
            // 이 시점엔 커밋되어 지속 상태가 이미 새 것
            // 사본은 그냥 틀림. 고치면 같은 사실의 두 번째 기록자
            if ( outcome->has_value() && ( *outcome )->result == EquipResult::Ok )
            {
                character_cache_.Invalidate( done_ctx, options_.server_id, request.character_id );
            }
            ++state->send_seq;
            atlas::SendFrame(
                *session, kOpEquipResponse, state->send_seq,
                EncodeEquipResponse( code, request.item_uid, static_cast< UInt8 >( request.slot ),
                                     unequipped ) );
        },
        PosterFor( session ) );

    if ( submitted != atlas::SubmitResult::Accepted )
    {
        // 새 결과 코드도 새 opcode 도 만들지 않음
        // 과부하 거부도 서비스에 닿기 전에 결정되는 거부임
        // 그래서 kEquipResponseUnavailable 을 그대로 씀
        // 연결은 유지. 과부하는 프로토콜 위반이 아니고 같이 벌하면 재접속 폭풍
        ++state->send_seq;
        atlas::SendFrame( *session, kOpEquipResponse, state->send_seq,
                          EncodeEquipResponse( kEquipResponseUnavailable, request.item_uid,
                                               static_cast< UInt8 >( request.slot ), 0 ) );
    }
}

void GameServer::HandleRanking( const std::shared_ptr< atlas::Session >& session,
                                const std::shared_ptr< SessionState >& state,
                                const atlas::Frame& frame )
{
    std::span< const Byte > cursor( frame.payload );
    UInt16 requested = 0;
    if ( !atlas::generated::ReadLe( cursor, requested ) )
    {
        ATLAS_LOG_WARN( "session {} sent a truncated ranking request - closing",
                        atlas::IdValue( session->Id() ) );
        session->Close();
        return;
    }

    // 캐시만으로 답하는 유일한 요청. 랭킹은 저장된 DB 형태가 없음
    // "Redis 다운"은 빈 리더보드가 아니라 Unavailable 로 가야 함
    ranking_.Top(
        MakeCtx( session, state ), options_.server_id, requested,
        [session, state]( bool ok, const std::vector< RankEntry >& entries )
        {
            ++state->send_seq;
            atlas::SendFrame( *session, kOpRankingResponse, state->send_seq,
                              EncodeRankingResponse(
                                  ok ? RankingResult::Ok : RankingResult::Unavailable, entries ) );
        },
        PosterFor( session ) );
}

}  // namespace atlas_demo
