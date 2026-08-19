#pragma once

// =============================================================================
// 상속축의 반대편. 필드와 타입만 있고 행동은 없음
// =============================================================================

#include <string>
#include <string_view>
#include <utility>

#include "atlas/core/ids.h"
#include "atlas/core/types.h"
#include "game/entity.h"

namespace atlas_demo
{

// [AD 7.3] 행동 트리와 서버 주도 AI 는 이 슬라이스에서 잘림
// 빈 Tick() 을 넣어 있는 척하지 않음
// 파생이 하나뿐인 Entity 는 가짜 기반 클래스. Npc 가 dispatch 를 실증
class Npc final : public Entity
{
public:
    Npc( atlas::ActorId id, atlas::UInt32 npc_id, atlas::Int32 pos_x, atlas::Int32 pos_y,
         atlas::UInt16 level, std::string name )
        : Entity( id, pos_x, pos_y, level ), npc_id_( npc_id ), name_( std::move( name ) )
    {
    }

    [[nodiscard]] EntityKind Kind() const noexcept override { return EntityKind::Npc; }
    [[nodiscard]] std::string_view DisplayName() const noexcept override { return name_; }

    // 스폰 원본인 정적 정의 참조
    // [AD 14] 해당 테이블은 info_generator 출력이라 아직 없음. 참조일 뿐
    [[nodiscard]] atlas::UInt32 NpcId() const noexcept { return npc_id_; }

private:
    atlas::UInt32 npc_id_;
    std::string name_;
};

}  // namespace atlas_demo
