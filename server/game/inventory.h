#pragma once
#include <cstddef>
#include <string_view>
#include <vector>

#include "atlas/core/ids.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "generated/db/character_items_row.h"

// The demo game's inventory / equipment domain (architecture-design.md §3.3 minimum set).
//
// 🔴 This file could not be written under server/atlas/**: `item`, `inventory` and `equip` are all
// on tools/core_purity/denylist.txt, so the gate (§15.4) would fail the build. That is the boundary
// working, not an obstacle to route around.
//
// 🔴 This file also owns the two prepared statements the generator does not emit. `db_generator`
// emits CRUD by primary key; "every item of one character" and "whatever occupies one slot" are
// game queries and belong to the game. They follow the same rule the runtime does
// (architecture-design.md §10): fixed text with `?` placeholders, never assembled at run time.

namespace atlas_demo {

// 🔴 Three slots, fixed by the demo minimum set (§3.3, and the same sentence sits in
// server/db/schema.json). `None` is the stored value 0 — "not equipped" — which is why the
// uniqueness invariant below can only ever be an application rule.
enum class EquipSlot : atlas::UInt8 { None = 0, Weapon = 1, Armor = 2, Trinket = 3 };

inline constexpr atlas::UInt8 kEquipSlotCount = 3;

// True for a slot a caller may actually equip into. 🔴 `None` is not one of them: "equip into slot
// 0" is an unequip request wearing an equip request's clothes, and letting it through would make
// the two indistinguishable in the log.
[[nodiscard]] constexpr bool IsEquippableSlot(EquipSlot slot) noexcept {
    return static_cast<atlas::UInt8>(slot) >= 1U &&
           static_cast<atlas::UInt8>(slot) <= kEquipSlotCount;
}

[[nodiscard]] std::string_view DescribeSlot(EquipSlot slot) noexcept;

// One item a character holds. The domain view of `character_items`; the row struct stays at the
// persistence edge so the domain does not carry column metadata around.
struct Item {
    atlas::UInt64 item_uid{0};
    atlas::UInt32 item_id{0};
    atlas::UInt16 stack_count{1};
    EquipSlot slot{EquipSlot::None};

    [[nodiscard]] bool Equipped() const noexcept { return slot != EquipSlot::None; }

    friend bool operator==(const Item&, const Item&) = default;
};

// One character's items, in memory.
//
// 🔴 THE "ONE ITEM PER SLOT" RULE IS AN APPLICATION INVARIANT, NOT A DATABASE CONSTRAINT. MySQL has
// no partial unique index, so a UNIQUE that applies only where `equip_slot != 0` cannot be
// declared (the same note is in server/db/schema.json and in the generated header). This class
// enforces it for the in-memory copy; the durable half is enforced by the transaction in
// equip_service.h. Neither one alone is sufficient, and that is exactly why the tests exist.
class Inventory {
public:
    void Add(const Item& item);
    void Clear() noexcept { items_.clear(); }

    [[nodiscard]] std::size_t Size() const noexcept { return items_.size(); }
    [[nodiscard]] const std::vector<Item>& All() const noexcept { return items_; }

    // Non-owning observers, null when there is nothing to point at (cpp-style.md §4.4).
    [[nodiscard]] const Item* Find(atlas::UInt64 item_uid) const noexcept;
    [[nodiscard]] const Item* EquippedAt(EquipSlot slot) const noexcept;

    // Moves `item_uid` into `slot`, unequipping whatever was there. Returns false when the item is
    // not held or the slot is not equippable — an expected failure, so not an exception
    // (architecture-design.md §11.2a).
    bool Equip(atlas::UInt64 item_uid, EquipSlot slot) noexcept;

private:
    // A flat vector, not a map keyed by uid. A demo character holds a handful of items and a linear
    // scan beats a node per item; the day that stops being true the measurement will say so
    // (§11.3 — the same reasoning that keeps the lock-free queue out).
    std::vector<Item> items_;
};

// ── The persistence edge ─────────────────────────────────────────────────────────────────────
// 🔴 atlas/db is generic on purpose (§10.3): it binds columns as values and knows no table, because
// core-purity forbids the core from including a generated row header. The mapper that knows both
// therefore lives HERE, above the core. This is the seam that decision creates, made concrete.

[[nodiscard]] Item ItemFromRow(const atlas::generated::CharacterItemsRow& row);

// Reads a result row selected in the generated column order into the generated row struct.
[[nodiscard]] atlas::generated::CharacterItemsRow CharacterItemsRowFromDb(const atlas::DbRow& row);

// Binding parameters for `kCharacterItemsUpdateByPkSql`, in the generated binding order.
// 🔴 The key columns bind LAST in an UPDATE, which is precisely why the generated binding array is
// walked instead of the column order being assumed.
[[nodiscard]] std::vector<atlas::DbValue> CharacterItemsUpdateParameters(
    const atlas::generated::CharacterItemsRow& row);

// Every item of one character, in the generated column order so the mapper above can read it.
inline constexpr std::string_view kSelectItemsByCharacterSql =
    "SELECT `server_id`, `character_id`, `item_uid`, `item_id`, `stack_count`, `equip_slot` "
    "FROM `character_items` WHERE `server_id` = ? AND `character_id` = ?";

// 🔴 One item by (server, uid) — deliberately WITHOUT `character_id` in the WHERE clause. Scoping
// the lookup to the requesting character would make "someone else's item" indistinguishable from
// "no such item", and §8.2 layer 3 (server authority) is precisely the check that the requester
// owns what it is asking about. The ownership test belongs in the application, where it can be
// answered and logged, not hidden in a predicate.
inline constexpr std::string_view kSelectItemByUidSql =
    "SELECT `server_id`, `character_id`, `item_uid`, `item_id`, `stack_count`, `equip_slot` "
    "FROM `character_items` WHERE `server_id` = ? AND `item_uid` = ?";

// Whatever currently occupies one slot. Served by idx_character_items_equip_slot.
inline constexpr std::string_view kSelectItemsInSlotSql =
    "SELECT `server_id`, `character_id`, `item_uid`, `item_id`, `stack_count`, `equip_slot` "
    "FROM `character_items` "
    "WHERE `server_id` = ? AND `character_id` = ? AND `equip_slot` = ?";

}  // namespace atlas_demo
