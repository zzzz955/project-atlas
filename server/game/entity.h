#pragma once

// =============================================================================
// [AD 3.3] 데모 게임의 도메인 모델. server/atlas/** 밖에 두는 이유는
// core-purity 게이트가 코어에서 item/inventory/equip 을 금지하기 때문
// =============================================================================

#include <string_view>

#include "atlas/core/ids.h"
#include "atlas/core/types.h"

namespace atlas_demo
{

// =============================================================================
// 소유축과 상속축
// =============================================================================

// 소유: Account (1) - Character (N). 상속: Entity -> Character / Npc
// [AD 6] 계정 하나가 여러 서버에 여러 캐릭터를 가짐
// Character : Account 로 묶으면 계정 상태가 캐릭터마다 복제됨
// 소유는 필드와 조회일 뿐

// dynamic_cast 없이 분기해야 하는 자리를 위한 구체 종류
enum class EntityKind : atlas::UInt8
{
    Character,
    Npc
};

// 월드에 존재하는 것의 최소 공통분모: 식별자 - 좌표 - 레벨
// [AD 7] Actor(AoI 등록 - tick - 행동)는 WORLD 관심사라 여기 없음
// 이 클래스를 키우는 것이 데모 최소 집합이 새는 경로
class Entity
{
public:
    virtual ~Entity() = default;

    Entity( const Entity& ) = delete;
    Entity& operator=( const Entity& ) = delete;
    Entity( Entity&& ) = delete;
    Entity& operator=( Entity&& ) = delete;

    [[nodiscard]] atlas::ActorId Id() const noexcept { return id_; }
    [[nodiscard]] atlas::Int32 PosX() const noexcept { return pos_x_; }
    [[nodiscard]] atlas::Int32 PosY() const noexcept { return pos_y_; }
    [[nodiscard]] atlas::UInt16 Level() const noexcept { return level_; }

    void SetPosition( atlas::Int32 pos_x, atlas::Int32 pos_y ) noexcept
    {
        pos_x_ = pos_x;
        pos_y_ = pos_y;
    }

    void SetLevel( atlas::UInt16 level ) noexcept { level_ = level; }

    // 파생 타입마다 다르게 답하는 둘. 파생이 하나뿐이면 죽은 virtual 이 됨
    [[nodiscard]] virtual EntityKind Kind() const noexcept = 0;
    [[nodiscard]] virtual std::string_view DisplayName() const noexcept = 0;

protected:
    Entity( atlas::ActorId id, atlas::Int32 pos_x, atlas::Int32 pos_y,
            atlas::UInt16 level ) noexcept
        : id_( id ), pos_x_( pos_x ), pos_y_( pos_y ), level_( level )
    {
    }

private:
    atlas::ActorId id_;
    atlas::Int32 pos_x_;
    atlas::Int32 pos_y_;
    atlas::UInt16 level_;
};

}  // namespace atlas_demo
