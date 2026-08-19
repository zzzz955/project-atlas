// =============================================================================
// 인벤토리 메모리 연산과 character_items row 매핑
// =============================================================================

#include "game/inventory.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <variant>
#include <vector>

#include "atlas/core/error.h"
#include "atlas/core/ids.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "generated/db/character_items_row.h"

namespace atlas_demo
{

namespace
{

using atlas::DbValue;
using atlas::UInt64;

// [AD 10.3] 드라이버가 전부 UInt64 로 넘기므로 좁히는 것이 매퍼의 몫
// 기대한 대안이 아니면 문 - 스키마 불일치라 예외가 맞음
// [CS 2.2] 이름에 테이블명을 실어 unity build TU 병합 시 충돌을 피함
UInt64 ReadItemUnsigned( const atlas::DbRow& row, std::size_t column )
{
    ATLAS_CHECK( column < row.size(), "character_items row is missing column {}", column );
    const UInt64* value = std::get_if< UInt64 >( &row[column] );
    ATLAS_CHECK( value != nullptr, "character_items column {} is not an unsigned value", column );
    return *value;
}

}  // namespace

std::string_view DescribeSlot( EquipSlot slot ) noexcept
{
    switch ( slot )
    {
        case EquipSlot::None:
            return "none";
        case EquipSlot::Weapon:
            return "weapon";
        case EquipSlot::Armor:
            return "armor";
        case EquipSlot::Trinket:
            return "trinket";
    }
    return "unknown";
}

void Inventory::Add( const Item& item ) { items_.push_back( item ); }

const Item* Inventory::Find( atlas::UInt64 item_uid ) const noexcept
{
    const auto found = std::ranges::find_if(
        items_, [item_uid]( const Item& held ) { return held.item_uid == item_uid; } );
    return found == items_.end() ? nullptr : &*found;
}

const Item* Inventory::EquippedAt( EquipSlot slot ) const noexcept
{
    if ( slot == EquipSlot::None )
    {
        return nullptr;
    }
    const auto found =
        std::ranges::find_if( items_, [slot]( const Item& held ) { return held.slot == slot; } );
    return found == items_.end() ? nullptr : &*found;
}

bool Inventory::Equip( atlas::UInt64 item_uid, EquipSlot slot ) noexcept
{
    if ( !IsEquippableSlot( slot ) )
    {
        return false;
    }
    const auto target = std::ranges::find_if(
        items_, [item_uid]( const Item& held ) { return held.item_uid == item_uid; } );
    if ( target == items_.end() )
    {
        return false;
    }
    // 해제가 먼저이고 첫 번째가 아니라 그 슬롯의 모든 점유자를 비움
    // 제약이 없는 불변식이라 고쳐야 할 상태를 성립한 것으로 가정하면 안 됨
    for ( Item& held : items_ )
    {
        if ( held.slot == slot )
        {
            held.slot = EquipSlot::None;
        }
    }
    target->slot = slot;
    return true;
}

Item ItemFromRow( const atlas::generated::CharacterItemsRow& row )
{
    return Item{ .item_uid = row.item_uid_,
                 .item_id = row.item_id_,
                 .stack_count = row.stack_count_,
                 .slot = static_cast< EquipSlot >( row.equip_slot_ ) };
}

atlas::generated::CharacterItemsRow CharacterItemsRowFromDb( const atlas::DbRow& row )
{
    atlas::generated::CharacterItemsRow mapped;
    mapped.server_id_ = static_cast< atlas::UInt16 >(
        ReadItemUnsigned( row, atlas::generated::kCharacterItemsColServerId ) );
    mapped.character_id_ = static_cast< atlas::CharacterId >(
        ReadItemUnsigned( row, atlas::generated::kCharacterItemsColCharacterId ) );
    mapped.item_uid_ = ReadItemUnsigned( row, atlas::generated::kCharacterItemsColItemUid );
    mapped.item_id_ = static_cast< atlas::UInt32 >(
        ReadItemUnsigned( row, atlas::generated::kCharacterItemsColItemId ) );
    mapped.stack_count_ = static_cast< atlas::UInt16 >(
        ReadItemUnsigned( row, atlas::generated::kCharacterItemsColStackCount ) );
    mapped.equip_slot_ = static_cast< atlas::UInt8 >(
        ReadItemUnsigned( row, atlas::generated::kCharacterItemsColEquipSlot ) );
    return mapped;
}

std::vector< atlas::DbValue > CharacterItemsUpdateParameters(
    const atlas::generated::CharacterItemsRow& row )
{
    // 생성 컬럼 순서로 담고 생성 바인딩 순서로 재배열
    // 두 단계로 나누면 SET / WHERE 분할을 이 함수가 알 필요가 없음
    const std::array< DbValue, atlas::generated::kCharacterItemsColumnCount > by_column{
        DbValue{ static_cast< UInt64 >( row.server_id_ ) },
        DbValue{ atlas::IdValue( row.character_id_ ) },
        DbValue{ row.item_uid_ },
        DbValue{ static_cast< UInt64 >( row.item_id_ ) },
        DbValue{ static_cast< UInt64 >( row.stack_count_ ) },
        DbValue{ static_cast< UInt64 >( row.equip_slot_ ) } };

    std::vector< DbValue > parameters;
    parameters.reserve( atlas::generated::kCharacterItemsUpdateByPkBinding.size() );
    for ( const std::size_t column : atlas::generated::kCharacterItemsUpdateByPkBinding )
    {
        parameters.push_back( by_column[column] );
    }
    return parameters;
}

}  // namespace atlas_demo
