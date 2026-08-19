#pragma once

// =============================================================================
// [AD 5.1] GAME 서버: opcode 테이블 - 핸들러 - 프로세스 배선
// 코어는 콜백만 받고 opcode 의 의미는 게임이 소유
// =============================================================================

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/db/connection_pool.h"
#include "atlas/db/db_runner.h"
#include "atlas/net/acceptor.h"
#include "atlas/net/io_runner.h"
#include "atlas/net/net_types.h"
#include "atlas/net/session.h"
#include "atlas/proto/frame.h"
#include "atlas/redis/connection.h"
#include "atlas/redis/redis_runner.h"
#include "game/character_cache.h"
#include "game/equip_service.h"
#include "game/inventory.h"
#include "game/ranking.h"
#include "generated/db/characters_row.h"

namespace atlas_demo
{

// =============================================================================
// 이 게임이 말하는 와이어 계약
// =============================================================================

// [AD 8.1] 프레임 계층은 UInt16 을 나를 뿐이고 opcode 의 의미는 게임이 소유
// shared/contracts 에 소스가 없어 손으로 씀. 넣으면 pkt_generator 재실행
// [AD 8.5] 페이로드는 생성 codec 헬퍼를 써서 바이트 규칙이 한 곳에서 옴
inline constexpr atlas::UInt16 kOpCharacterLoadRequest = 0x0101;
inline constexpr atlas::UInt16 kOpCharacterLoadResponse = 0x0102;
inline constexpr atlas::UInt16 kOpEquipRequest = 0x0201;
inline constexpr atlas::UInt16 kOpEquipResponse = 0x0202;
inline constexpr atlas::UInt16 kOpRankingRequest = 0x0301;
inline constexpr atlas::UInt16 kOpRankingResponse = 0x0302;

// kOpCharacterLoadResponse 의 결과 바이트
enum class LoadResult : atlas::UInt8
{
    Ok = 0,
    NotFound,
    // [AD 10.3] 획득 타임아웃 안에 DB 풀이 고갈. 멈춤이 아니라 분명한 실패
    Unavailable,
};

// kOpEquipResponse 의 결과 바이트
// 서비스를 부르기 전에 결정되는 거부 둘을 EquipResult 에 덧붙임
inline constexpr atlas::UInt8 kEquipResponseNotLoaded = 0xFE;
inline constexpr atlas::UInt8 kEquipResponseUnavailable = 0xFF;

// kOpRankingResponse 의 결과 바이트
enum class RankingResult : atlas::UInt8
{
    Ok = 0,
    // "랭킹이 비었음"이 아님
    // 로드 경로는 DB 가 대신 답해 조용히 저하되지만 이쪽은 그럴 수 없음
    // 그렇게 말하는 것이 "사본일 뿐"의 정직한 절반
    Unavailable,
};

// =============================================================================
// 연결별 게임 상태
// =============================================================================

// [AD 9.1] 모든 필드는 세션 strand 에서만 접근
// 프레임 핸들러가 거기서 돌고 DB 완료도 세션 executor 로 되돌아옴
// 그래서 이 구조체에 락이 없고 send_seq 도 평범한 카운터로 충분함
struct SessionState
{
    atlas::UInt16 server_id{ 0 };
    atlas::CharacterId character_id{};
    bool loaded{ false };
    // [AD 8.3] 방향당 송신 카운터 1개. SendFrame 안에 숨기지 않고 메시지 계층이 소유
    atlas::UInt32 send_seq{ 0 };
};

// 로드 결과. DB 스레드에서 세션 strand 로 건너감
struct CharacterSnapshot
{
    LoadResult result{ LoadResult::NotFound };
    atlas::generated::CharactersRow character{};
    std::vector< Item > items{};
};

// 커밋을 원할 때 실패시키는 이음매
// EquipService::FaultInjector 와 같은 이유로 프로덕션 시그니처에 있음
// 커밋을 막을 수 없으면 "커밋 뒤에만 쓴다"를 밖에서 관측 불가
// 프로덕션에서는 비어 있음
using LoadFaultInjector = std::function< void() >;

// DB 스레드에서 임대 커넥션으로 실행. 읽기 -> 로그인 기록 -> 커밋 -> ZADD
// ZADD 가 호출자 완료가 아니라 Commit() 뒤 같은 스코프에 있는 것이 보장 전부
// [CS 4.4] ranking 은 비소유 관찰자이고 캐시 미설정이면 null 일 수 있음
void LoadCharacterOnDbThread( atlas::Ctx& ctx, atlas::Connection& connection,
                              atlas::UInt16 server_id, atlas::CharacterId character_id,
                              CharacterSnapshot& out, ExpRanking* ranking,
                              const LoadFaultInjector& before_commit = {} );

// =============================================================================
// 디스패치 테이블
// =============================================================================

class HandlerTable
{
public:
    using Handler =
        std::function< void( const std::shared_ptr< atlas::Session >&,
                             const std::shared_ptr< SessionState >&, const atlas::Frame& ) >;

    void Register( atlas::UInt16 opcode, Handler handler );

    // 비소유 관찰자. null 은 "이 게임이 모르는 opcode"
    [[nodiscard]] const Handler* Find( atlas::UInt16 opcode ) const noexcept;

    [[nodiscard]] std::size_t Size() const noexcept { return handlers_.size(); }

private:
    std::unordered_map< atlas::UInt16, Handler > handlers_;
};

// =============================================================================
// 서버
// =============================================================================

// 배선이 main.cpp 가 아니라 핸들러 옆에 있는 이유
// main.cpp 는 테스트 실행 파일에 링크되지 않음
// 이 슬라이스의 종단 증명은 실제 소켓으로 로드 -> 장착 -> 재접속을 도는 테스트
// main 이 배선을 쥐면 테스트가 다른 서버를 짬
class GameServer
{
public:
    struct Options
    {
        atlas::Endpoint endpoint{};
        atlas::UInt16 server_id{ 1 };
        // [AD 9] 0 = std::thread::hardware_concurrency
        std::size_t io_workers{ 0 };
        std::size_t db_pool_size{ 4 };
        std::size_t db_threads{ 2 };
    };

    // 리스닝 소켓과 DB 커넥션을 전부 즉시 엶
    // 못 잡는 포트나 안 되는 자격은 첫 요청의 놀람이 아니라 기동 실패
    // [AD 10.2] 캐시는 정반대. host 가 비면 "캐시 없음"이고 서버는 그대로 돎
    GameServer( const Options& options, const atlas::DbConnectionConfig& db_config,
                const atlas::RedisConnectionConfig& redis_config = {} );
    ~GameServer();

    GameServer( const GameServer& ) = delete;
    GameServer& operator=( const GameServer& ) = delete;
    GameServer( GameServer&& ) = delete;
    GameServer& operator=( GameServer&& ) = delete;

    void Start();

    // io_runner.Stop() 은 work guard 만 놓음
    // 앞문을 먼저 닫아야 큐가 유한하게 빠짐
    // DB 풀을 세션과 I/O 사이에서 비워야 커밋된 작업이 뜨지 않음

    // [AD 9] 순서가 계약이고 멱등함
    // acceptor.Stop() -> 세션 Close() -> db_runner.Stop() -> redis.Stop() -> io_runner.Stop()
    // 캐시가 I/O 풀보다 먼저인 이유는 재접속 루프가 미결 작업이기 때문
    void Stop() noexcept;

    // [AD 16.1] 서버가 자기 자신에 대해 아는 것
    // 밖에서는 거부를 셀 수 없음
    // 클라이언트는 "unavailable" 만 보고 큐 만석과 풀 고갈을 구분 못 함
    // 노출은 지표 엔드포인트 없이 main.cpp 의 로그 한 줄
    struct Counters
    {
        std::size_t live_sessions{ 0 };
        std::size_t db_queue_depth{ 0 };
        std::size_t db_queue_capacity{ 0 };
        atlas::UInt64 db_rejected{ 0 };
        atlas::UInt64 db_acquire_failures{ 0 };
        atlas::UInt64 db_acquire_wait_micros{ 0 };
    };

    [[nodiscard]] const atlas::Endpoint& LocalEndpoint() const noexcept;
    // 세션 수는 atlas/net 이 아니라 이 레지스트리에서
    // 코어는 세션을 나눠줄 뿐 인구조사를 하지 않음
    // 네트워크 계층에 물으면 같은 답이 두 곳에 생김
    [[nodiscard]] std::size_t LiveSessionCount() const;
    [[nodiscard]] Counters ReadCounters() const;
    [[nodiscard]] const HandlerTable& Handlers() const noexcept { return handlers_; }

private:
    void RegisterHandlers();
    void OnAccept( const std::shared_ptr< atlas::Session >& session );
    void Dispatch( const std::shared_ptr< atlas::Session >& session,
                   const std::shared_ptr< SessionState >& state, const atlas::Frame& frame );

    void HandleCharacterLoad( const std::shared_ptr< atlas::Session >& session,
                              const std::shared_ptr< SessionState >& state,
                              const atlas::Frame& frame );
    void HandleEquip( const std::shared_ptr< atlas::Session >& session,
                      const std::shared_ptr< SessionState >& state, const atlas::Frame& frame );
    void HandleRanking( const std::shared_ptr< atlas::Session >& session,
                        const std::shared_ptr< SessionState >& state, const atlas::Frame& frame );

    // 캐시가 답한 뒤 들어오는 로드의 후반부
    // cached 가 비면 미스(또는 Redis 무응답)라 DB 가 전부 해야 함
    void SubmitCharacterLoad( const std::shared_ptr< atlas::Session >& session,
                              const std::shared_ptr< SessionState >& state, const atlas::Ctx& ctx,
                              atlas::CharacterId character_id,
                              std::optional< CachedCharacter > cached );

    void Forget( atlas::SessionId id );
    [[nodiscard]] atlas::Ctx MakeCtx( const std::shared_ptr< atlas::Session >& session,
                                      const std::shared_ptr< SessionState >& state );
    [[nodiscard]] static atlas::DbRunner::CompletionPoster PosterFor(
        const std::shared_ptr< atlas::Session >& session );

    Options options_;

    // 선언 순서가 소멸 계약. 풀은 임대하는 runner 보다 오래 살아야 함
    // io_context 는 acceptor 와 거기 묶인 모든 세션보다 오래 살아야 함
    atlas::ConnectionPool pool_;
    atlas::DbRunner db_runner_;
    atlas::IoRunner io_runner_;
    // io_runner_ 뒤에 선언해 먼저 소멸시킴. 이 연결은 그 io_context 위에 삶
    // 자기가 post 하는 context 보다 오래 사는 객체가 이 계층의 유일한 순서 실수
    // 풀이 없는 이유는 boost-redis 가 도는 스레드를 막지 않기 때문
    atlas::RedisConnection redis_;
    atlas::RedisRunner redis_runner_;
    std::unique_ptr< atlas::SessionAcceptor > acceptor_;

    EquipService equip_service_;
    ExpRanking ranking_;
    CharacterCache character_cache_;
    HandlerTable handlers_;

    // 캐시가 설정되었는지. Start() 만 여기서 분기함
    // 나머지는 캐시에 묻고 "아니오"를 처리함
    // 그래서 "Redis 다운"과 "Redis 없음"이 한 경로
    bool redis_enabled_{ false };

    // [AD 9.1] 락은 진짜 공유 자원에만
    // accept strand 가 넣고 N개 세션 strand 가 지우고 종료 스레드가 순회함
    // 어느 strand 도 직렬화하지 못함
    // weak_ptr 인 이유는 레지스트리가 닫힌 세션을 살려두면 안 되기 때문
    mutable std::mutex sessions_mutex_;
    std::unordered_map< atlas::UInt64, std::weak_ptr< atlas::Session > > sessions_;

    std::atomic< atlas::UInt64 > next_trace_id_{ 1 };
    bool running_{ false };
};

}  // namespace atlas_demo
