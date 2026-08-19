#pragma once

// =============================================================================
// [AD 3.3] 데모 게임의 인벤토리 / 장비 도메인과 그 영속화 경계
// [AD 15.4] item - inventory - equip 은 denylist 라 코어에 못 쓰는 코드
// =============================================================================

#include <cstddef>
#include <string_view>
#include <vector>

#include "atlas/core/ids.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "generated/db/character_items_row.h"

namespace atlas_demo
{

// [AD 3.3] 슬롯 3개 고정. schema.json 에도 같은 문장이 있음
// None 은 저장값 0 = 미장착
enum class EquipSlot : atlas::UInt8
{
    None = 0,
    Weapon = 1,
    Armor = 2,
    Trinket = 3
};

inline constexpr atlas::UInt8 kEquipSlotCount = 3;

// 실제로 장착 가능한 슬롯인지. None 은 여기 포함되지 않음
// "슬롯 0 에 장착"은 해제 요청이 장착 요청 옷을 입은 것이라 로그에서 구분 불가
[[nodiscard]] constexpr bool IsEquippableSlot( EquipSlot slot ) noexcept
{
    return static_cast< atlas::UInt8 >( slot ) >= 1U &&
           static_cast< atlas::UInt8 >( slot ) <= kEquipSlotCount;
}

[[nodiscard]] std::string_view DescribeSlot( EquipSlot slot ) noexcept;

// 캐릭터가 든 아이템 1개
// row 구조체는 영속화 경계에 남겨 도메인이 컬럼 메타를 들고 다니지 않게 함
struct Item
{
    atlas::UInt64 item_uid{ 0 };
    atlas::UInt32 item_id{ 0 };
    atlas::UInt16 stack_count{ 1 };
    EquipSlot slot{ EquipSlot::None };

    [[nodiscard]] bool Equipped() const noexcept { return slot != EquipSlot::None; }

    friend bool operator==( const Item&, const Item& ) = default;
};

// 한 캐릭터의 아이템, 메모리 사본
// 슬롯당 1개는 애플리케이션 불변식
// MySQL 에 부분 유니크 인덱스가 없어 equip_slot != 0 조건부 UNIQUE 선언 불가
// 지속 쪽은 equip_service 가 맡음
class Inventory
{
public:
    void Add( const Item& item );
    void Clear() noexcept { items_.clear(); }

    [[nodiscard]] std::size_t Size() const noexcept { return items_.size(); }
    [[nodiscard]] const std::vector< Item >& All() const noexcept { return items_; }

    // [CS 4.4] 비소유 관찰자. 가리킬 것이 없으면 null
    [[nodiscard]] const Item* Find( atlas::UInt64 item_uid ) const noexcept;
    [[nodiscard]] const Item* EquippedAt( EquipSlot slot ) const noexcept;

    // item_uid 를 slot 으로 옮기고 기존 점유자는 해제
    // [AD 11.2a] 미보유 - 장착 불가 슬롯은 예상된 실패라 예외가 아닌 false
    bool Equip( atlas::UInt64 item_uid, EquipSlot slot ) noexcept;

private:
    // uid 맵이 아니라 평평한 vector. 데모 캐릭터는 아이템이 몇 개뿐
    // [AD 11.3] 선형 탐색이 아이템당 노드보다 나음. 바뀌면 측정이 먼저 말함
    std::vector< Item > items_;
};

// =============================================================================
// 영속화 경계
// =============================================================================

// [AD 10.3] atlas/db 는 컬럼을 값으로 바인딩할 뿐 테이블을 모름
// core-purity 가 코어의 생성 헤더 include 를 막으므로 매퍼는 코어 위 여기에 있음

[[nodiscard]] Item ItemFromRow( const atlas::generated::CharacterItemsRow& row );

[[nodiscard]] atlas::generated::CharacterItemsRow CharacterItemsRowFromDb(
    const atlas::DbRow& row );

// UPDATE 는 키 컬럼이 마지막에 바인딩됨
// 컬럼 순서를 가정하지 않고 생성된 바인딩 배열을 그대로 순회
[[nodiscard]] std::vector< atlas::DbValue > CharacterItemsUpdateParameters(
    const atlas::generated::CharacterItemsRow& row );

// [AD 10] db_generator 는 기본키 CRUD 만 냄. 게임 질의는 게임이 소유
// 런타임 조립 없이 ? 자리표시자 고정 텍스트라는 규칙은 같음

// 한 캐릭터의 모든 아이템. 위 매퍼가 읽도록 생성된 컬럼 순서
inline constexpr std::string_view kSelectItemsByCharacterSql =
    "SELECT `server_id`, `character_id`, `item_uid`, `item_id`, `stack_count`, `equip_slot` "
    "FROM `character_items` WHERE `server_id` = ? AND `character_id` = ?";

// (server, uid) 로 아이템 1개. WHERE 에 character_id 를 일부러 넣지 않음
// [AD 8.2] 조회를 요청자로 좁히면 "남의 아이템"과 "없는 아이템"이 구분되지 않음
// 소유 검사는 답하고 로그로 남길 수 있는 애플리케이션의 몫
inline constexpr std::string_view kSelectItemByUidSql =
    "SELECT `server_id`, `character_id`, `item_uid`, `item_id`, `stack_count`, `equip_slot` "
    "FROM `character_items` WHERE `server_id` = ? AND `item_uid` = ?";

// 현재 그 슬롯을 점유한 것. idx_character_items_equip_slot 이 처리
inline constexpr std::string_view kSelectItemsInSlotSql =
    "SELECT `server_id`, `character_id`, `item_uid`, `item_id`, `stack_count`, `equip_slot` "
    "FROM `character_items` "
    "WHERE `server_id` = ? AND `character_id` = ? AND `equip_slot` = ?";

}  // namespace atlas_demo
