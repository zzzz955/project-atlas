#pragma once

// =============================================================================
// [AD 10.2] 데모 게임의 캐릭터 읽기 캐시
// 읽기는 read-through, 쓰기는 무효화. write-through 가 아님
// =============================================================================

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"
#include "atlas/core/types.h"
#include "atlas/redis/redis_runner.h"
#include "game/inventory.h"
#include "generated/db/characters_row.h"

namespace atlas_demo
{

// `character:{server_id}:{character_id}`
// [AD 6] 기본키와 같은 범위라 서버 간 키 충돌이 구조상 불가능
[[nodiscard]] std::string CharacterCacheKey( atlas::UInt16 server_id,
                                             atlas::CharacterId character_id );

// 웜 로드가 답할 수 있는 상태 그대로
struct CachedCharacter
{
    atlas::generated::CharactersRow character{};
    std::vector< Item > items{};
};

// 저장 값은 생성된 codec 헬퍼로 만든 게임 자체 리틀엔디언 인코딩
// [AD 8.5] 바이트 규칙 구현을 이 저장소에 둘로 늘리지 않기 위함
// 와이어 응답이 아님. 응답을 캐시하면 저장 형식이 프로토콜에 묶임
[[nodiscard]] std::string EncodeCachedCharacter( const CachedCharacter& value );

// 디코드되지 않으면 nullopt. 옛 빌드가 남긴 값은 오류가 아니라 미스
[[nodiscard]] std::optional< CachedCharacter > DecodeCachedCharacter( std::string_view encoded );

// 웜 경로의 로그인 시각 기록
// 캐시 사본으로 생성 UPDATE 를 쓰면 사본 시점의 모든 컬럼이 되살아남
// 그래서 로그인이 소유한 한 컬럼만 건드림
inline constexpr std::string_view kTouchCharacterLoginSql =
    "UPDATE `characters` SET `last_login_at` = ? WHERE `server_id` = ? AND `character_id` = ?";

// write-through 는 쓰는 동안 사본을 권위로 만듦
// 그 창에서 죽으면 어느 쪽이 진짜인지 알 수 없는 답 두 개가 남음
// 무효화는 그 상태를 만들 수 없음
// 최악이라야 다음 독자가 DB 에 다시 묻는 것뿐
class CharacterCache
{
public:
    // 상수 하나. 웜 응답이 얼마나 낡을 수 있는지의 상한이자 유일한 정책
    // 키별 정책 - 워밍 - 부분 무효화 - 네거티브 캐시는 전부 범위 밖
    static constexpr atlas::UInt32 kTtlSeconds = 60;

    // nullopt = "DB 에 물어라"
    // 진짜 미스 - 디코드 실패 - Redis 불통이 모두 같은 모양으로 옴
    // Redis 다운은 이 서버의 장애가 아니라 미스일 뿐
    using GetHandler = std::function< void( std::optional< CachedCharacter > cached ) >;

    explicit CharacterCache( atlas::RedisRunner& runner ) : runner_( &runner ) {}

    void Get( atlas::Ctx ctx, atlas::UInt16 server_id, atlas::CharacterId character_id,
              GetHandler handler, atlas::RedisRunner::CompletionPoster poster = {} );

    // TTL 과 함께 SET. 완료 콜백 없음
    // 사본 쓰기 실패는 방금 전 시스템이 이미 있던 상태
    void Put( atlas::Ctx ctx, atlas::UInt16 server_id, const CachedCharacter& value );

    // DEL. 커밋된 뒤에 호출할 것(ranking.h 와 같은 이유)
    // 커밋 전에 무효화하면 커밋 전 행으로 키를 다시 채운 독자에게 짐
    // 그 낡은 사본이 TTL 을 다 살아감
    void Invalidate( atlas::Ctx ctx, atlas::UInt16 server_id, atlas::CharacterId character_id );

private:
    // [CS 4.4] 비소유 관찰자. runner 는 구조상 이 객체보다 오래 삶
    atlas::RedisRunner* runner_;
};

}  // namespace atlas_demo
