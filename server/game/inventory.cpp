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

namespace atlas_demo {

namespace {

using atlas::DbValue;
using atlas::UInt64;

// Every unsigned column arrives from the driver as UInt64 and every signed one as Int64 (§10.3);
// narrowing back to the declared width is the mapper's job, and the generated ColumnMeta is what
// makes the width known. A row that does not hold the expected alternative is a statement/schema
// mismatch, i.e. a programming error — the half of §11.2(a) that really is an exception.
//
// 🔴 The name carries the table because with unity build ON several sources share one translation
// unit, and two anonymous-namespace helpers with the same name then collide (cpp-style.md §2.2).
UInt64 ReadItemUnsigned(const atlas::DbRow& row, std::size_t column) {
    ATLAS_CHECK(column < row.size(), "character_items row is missing column {}", column);
    const UInt64* value = std::get_if<UInt64>(&row[column]);
    ATLAS_CHECK(value != nullptr, "character_items column {} is not an unsigned value", column);
    return *value;
}

}  // namespace

std::string_view DescribeSlot(EquipSlot slot) noexcept {
    switch (slot) {
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

void Inventory::Add(const Item& item) { items_.push_back(item); }

const Item* Inventory::Find(atlas::UInt64 item_uid) const noexcept {
    const auto found = std::find_if(items_.begin(), items_.end(), [item_uid](const Item& held) {
        return held.item_uid == item_uid;
    });
    return found == items_.end() ? nullptr : &*found;
}

const Item* Inventory::EquippedAt(EquipSlot slot) const noexcept {
    if (slot == EquipSlot::None) {
        return nullptr;
    }
    const auto found = std::find_if(items_.begin(), items_.end(),
                                    [slot](const Item& held) { return held.slot == slot; });
    return found == items_.end() ? nullptr : &*found;
}

bool Inventory::Equip(atlas::UInt64 item_uid, EquipSlot slot) noexcept {
    if (!IsEquippableSlot(slot)) {
        return false;
    }
    const auto target = std::find_if(items_.begin(), items_.end(), [item_uid](const Item& held) {
        return held.item_uid == item_uid;
    });
    if (target == items_.end()) {
        return false;
    }
    // 🔴 The unequip comes first and it clears EVERY holder of the slot, not just the first. The
    // uniqueness rule is an application invariant with no database constraint behind it, so this
    // code must not assume the state it is supposed to be repairing.
    for (Item& held : items_) {
        if (held.slot == slot) {
            held.slot = EquipSlot::None;
        }
    }
    target->slot = slot;
    return true;
}

Item ItemFromRow(const atlas::generated::CharacterItemsRow& row) {
    return Item{.item_uid = row.item_uid_,
                .item_id = row.item_id_,
                .stack_count = row.stack_count_,
                .slot = static_cast<EquipSlot>(row.equip_slot_)};
}

atlas::generated::CharacterItemsRow CharacterItemsRowFromDb(const atlas::DbRow& row) {
    atlas::generated::CharacterItemsRow mapped;
    mapped.server_id_ = static_cast<atlas::UInt16>(
        ReadItemUnsigned(row, atlas::generated::kCharacterItemsColServerId));
    mapped.character_id_ = static_cast<atlas::CharacterId>(
        ReadItemUnsigned(row, atlas::generated::kCharacterItemsColCharacterId));
    mapped.item_uid_ = ReadItemUnsigned(row, atlas::generated::kCharacterItemsColItemUid);
    mapped.item_id_ = static_cast<atlas::UInt32>(
        ReadItemUnsigned(row, atlas::generated::kCharacterItemsColItemId));
    mapped.stack_count_ = static_cast<atlas::UInt16>(
        ReadItemUnsigned(row, atlas::generated::kCharacterItemsColStackCount));
    mapped.equip_slot_ = static_cast<atlas::UInt8>(
        ReadItemUnsigned(row, atlas::generated::kCharacterItemsColEquipSlot));
    return mapped;
}

std::vector<atlas::DbValue> CharacterItemsUpdateParameters(
    const atlas::generated::CharacterItemsRow& row) {
    // Indexed by the generated column index, then permuted through the generated binding order.
    // Doing it in two steps is what keeps the SET/WHERE split out of this function entirely.
    const std::array<DbValue, atlas::generated::kCharacterItemsColumnCount> by_column{
        DbValue{static_cast<UInt64>(row.server_id_)},
        DbValue{atlas::IdValue(row.character_id_)},
        DbValue{row.item_uid_},
        DbValue{static_cast<UInt64>(row.item_id_)},
        DbValue{static_cast<UInt64>(row.stack_count_)},
        DbValue{static_cast<UInt64>(row.equip_slot_)}};

    std::vector<DbValue> parameters;
    parameters.reserve(atlas::generated::kCharacterItemsUpdateByPkBinding.size());
    for (const std::size_t column : atlas::generated::kCharacterItemsUpdateByPkBinding) {
        parameters.push_back(by_column[column]);
    }
    return parameters;
}

}  // namespace atlas_demo
