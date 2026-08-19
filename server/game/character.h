#pragma once

// =============================================================================
// [AD 6] 계정 - 캐릭터 소유 관계, 캐릭터 도메인, 그 영속화 경계
// =============================================================================

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "atlas/core/ids.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "game/entity.h"
#include "game/inventory.h"
#include "generated/db/characters_row.h"

namespace atlas_demo
{

// 소유 관계. 계정이 캐릭터 N개를 가짐. 상속이 아님(entity.h 참고)
// [AD 12] 계정 자체는 platform-auth 소유라 이 서버가 저장하지 않음
class Account
{
public:
    explicit Account( atlas::AccountId uid ) noexcept : uid_( uid ) {}

    [[nodiscard]] atlas::AccountId Uid() const noexcept { return uid_; }
    [[nodiscard]] const std::vector< atlas::CharacterId >& Characters() const noexcept
    {
        return characters_;
    }

    void Own( atlas::CharacterId character_id )
    {
        if ( !Owns( character_id ) )
        {
            characters_.push_back( character_id );
        }
    }

    [[nodiscard]] bool Owns( atlas::CharacterId character_id ) const noexcept
    {
        return std::find( characters_.begin(), characters_.end(), character_id ) !=
               characters_.end();
    }

private:
    atlas::AccountId uid_;
    // 객체가 아니라 id
    // [AD 10] 관계 자동 로딩 금지
    // 로그인이 계정의 모든 캐릭터를 실체화하는 뜻이 되면 안 됨
    std::vector< atlas::CharacterId > characters_;
};

// [AD 6] 신원 3계층의 마지막 단
// 기본키가 (server_id, character_id) 라 server_id 가 객체와 함께 다님
// 강타입 id 가 아닌 근거는 core/ids.h 에 있음
class Character final : public Entity
{
public:
    Character( atlas::UInt16 server_id, atlas::CharacterId character_id,
               atlas::AccountId account_uid, std::string name, atlas::Int32 pos_x,
               atlas::Int32 pos_y, atlas::UInt16 level, atlas::UInt64 exp );

    [[nodiscard]] EntityKind Kind() const noexcept override { return EntityKind::Character; }
    [[nodiscard]] std::string_view DisplayName() const noexcept override { return name_; }

    [[nodiscard]] atlas::UInt16 ServerId() const noexcept { return server_id_; }
    [[nodiscard]] atlas::CharacterId CharacterKey() const noexcept { return character_id_; }
    [[nodiscard]] atlas::AccountId AccountUid() const noexcept { return account_uid_; }
    [[nodiscard]] atlas::UInt64 Exp() const noexcept { return exp_; }
    void SetExp( atlas::UInt64 exp ) noexcept { exp_ = exp; }

    [[nodiscard]] Inventory& Items() noexcept { return items_; }
    [[nodiscard]] const Inventory& Items() const noexcept { return items_; }

private:
    atlas::UInt16 server_id_;
    atlas::CharacterId character_id_;
    atlas::AccountId account_uid_;
    std::string name_;
    atlas::UInt64 exp_;
    Inventory items_;
};

// =============================================================================
// 영속화 경계
// =============================================================================

// [AD 10.3] atlas/db 는 테이블을 모름. 생성 row 와 런타임을 둘 다 아는 코드가 여기

// DATETIME 은 소수부가 없어 마이크로초를 담으면 왕복이 깨짐
// [CS 4.2] 저장은 SysClock. 단조 시계는 타임아웃과 tick 용
[[nodiscard]] atlas::SysTime PersistableNow();

[[nodiscard]] Character CharacterFromRow( const atlas::generated::CharactersRow& row );

[[nodiscard]] atlas::generated::CharactersRow CharactersRowFromDb( const atlas::DbRow& row );

// 생성된 바인딩 순서를 그대로 따르는 UPDATE 파라미터
[[nodiscard]] std::vector< atlas::DbValue > CharactersUpdateParameters(
    const atlas::generated::CharactersRow& row );

}  // namespace atlas_demo
