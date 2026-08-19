#pragma once

// =============================================================================
// [AD 10.2] 데모 게임의 exp 랭킹. Redis 읽기 캐시 축
// DB 가 진실이고 여기는 사본. 유실은 SELECT 한 번으로 복구되는 값
// =============================================================================

#include <functional>
#include <string>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"
#include "atlas/core/types.h"
#include "atlas/redis/redis_runner.h"

namespace atlas_demo
{

// `ranking:{server_id}:exp` - 서버당 sorted set 1개
// 두 서버를 합치면 만날 일 없는 모집단을 비교하게 됨
// [AD 15.4] 키 형식은 게임 어휘. ranking 은 core-purity denylist 에 있음
[[nodiscard]] std::string ExpRankingKey( atlas::UInt16 server_id );

// 출처는 이미 있는 characters.exp
// [AD 3.3] 리더보드를 위해 새 컬럼이나 새 테이블을 만들지 않음
struct RankEntry
{
    atlas::CharacterId character_id{};
    atlas::UInt64 exp{ 0 };

    friend bool operator==( const RankEntry&, const RankEntry& ) = default;
};

// [AD 8.1] 페이로드 상한 16 KiB
// 무제한 ZREVRANGE 는 클라이언트가 서버 작업량과 응답 크기를 모두 고르게 함
inline constexpr atlas::UInt16 kMaxRankEntries = 100;

class ExpRanking
{
public:
    // ok=false 는 Redis 무응답. "랭킹이 비었음"이 아님
    // 둘을 섞으면 장애가 아무도 없는 리더보드로 보고됨
    using TopHandler = std::function< void( bool ok, const std::vector< RankEntry >& entries ) >;

    explicit ExpRanking( atlas::RedisRunner& runner ) : runner_( &runner ) {}

    // ZADD. 값을 읽은 트랜잭션이 커밋된 뒤에만 호출할 것
    // 먼저 쓰면 DB 가 아직 거부할 수 있는 수를 기록하고 되돌릴 수단이 없음
    // 완료 콜백 없음. 실패는 DEBUG 로그뿐이고 다음 로드가 값을 되돌림
    void Record( atlas::Ctx ctx, atlas::UInt16 server_id, atlas::CharacterId character_id,
                 atlas::UInt64 exp );

    // ZREVRANGE WITHSCORES, exp 내림차순. count 는 kMaxRankEntries 로 제한
    void Top( atlas::Ctx ctx, atlas::UInt16 server_id, atlas::UInt16 count, TopHandler handler,
              atlas::RedisRunner::CompletionPoster poster = {} );

private:
    // [CS 4.4] 비소유 관찰자. runner 는 구조상 이 객체보다 오래 삶
    atlas::RedisRunner* runner_;
};

}  // namespace atlas_demo
