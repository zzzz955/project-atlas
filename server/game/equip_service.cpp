#include "game/equip_service.h"

#include <array>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/db/prepared_statement.h"
#include "atlas/db/transaction.h"
#include "game/character.h"
#include "game/inventory.h"
#include "generated/db/character_items_row.h"
#include "generated/db/characters_row.h"

namespace atlas_demo {

namespace {

using atlas::DbRow;
using atlas::DbValue;
using atlas::UInt64;

}  // namespace

std::string_view DescribeEquipResult(EquipResult result) noexcept {
    switch (result) {
        case EquipResult::Ok:
            return "ok";
        case EquipResult::InvalidSlot:
            return "invalid slot";
        case EquipResult::ItemNotFound:
            return "no such item";
        case EquipResult::NotOwned:
            return "item belongs to another character";
        case EquipResult::CharacterNotFound:
            return "no such character";
    }
    return "unknown";
}

std::mutex& EquipService::LockFor(const CharacterKey& key) {
    const std::scoped_lock registry_guard(registry_mutex_);
    std::unique_ptr<std::mutex>& slot = character_locks_[key];
    if (!slot) {
        slot = std::make_unique<std::mutex>();
    }
    return *slot;
}

EquipService::Outcome EquipService::Equip(atlas::Ctx& ctx, atlas::Connection& connection,
                                          const Request& request,
                                          const FaultInjector& before_commit) {
    if (!IsEquippableSlot(request.slot)) {
        return {.result = EquipResult::InvalidSlot, .unequipped_item_uid = 0};
    }

    // 🔴 Held for the whole read-modify-write, not just the writes. The interleaving that breaks
    // the invariant is "both readers saw the slot free", so a lock that starts after the read is a
    // lock that protects nothing.
    const std::scoped_lock character_guard(
        LockFor(CharacterKey{request.server_id, atlas::IdValue(request.character_id)}));

    // ── Server authority (§8.2 layer 3) ──────────────────────────────────────────────────────
    // The item is looked up by (server, uid) and its owner is then COMPARED. Putting character_id
    // in the WHERE clause instead would collapse "someone else's item" into "no such item", and a
    // refusal nobody can tell apart from a typo is a refusal nobody investigates.
    const std::array<DbValue, 2> item_lookup{DbValue{static_cast<UInt64>(request.server_id)},
                                             DbValue{request.item_uid}};
    const std::vector<DbRow> item_rows = connection.Prepare(kSelectItemByUidSql).Query(item_lookup);
    if (item_rows.empty()) {
        return {.result = EquipResult::ItemNotFound, .unequipped_item_uid = 0};
    }

    atlas::generated::CharacterItemsRow target = CharacterItemsRowFromDb(item_rows.front());
    if (target.character_id_ != request.character_id) {
        ATLAS_LOG_WARN("equip refused: item {} belongs to character {}, not {}", target.item_uid_,
                       atlas::IdValue(target.character_id_), atlas::IdValue(request.character_id));
        return {.result = EquipResult::NotOwned, .unequipped_item_uid = 0};
    }

    const std::array<DbValue, 2> character_lookup{DbValue{static_cast<UInt64>(request.server_id)},
                                                  DbValue{atlas::IdValue(request.character_id)}};
    const std::vector<DbRow> character_rows =
        connection.Prepare(atlas::generated::kCharactersSelectByPkSql).Query(character_lookup);
    if (character_rows.empty()) {
        return {.result = EquipResult::CharacterNotFound, .unequipped_item_uid = 0};
    }
    atlas::generated::CharactersRow character = CharactersRowFromDb(character_rows.front());

    // ── One transaction, three writes, two tables ────────────────────────────────────────────
    // 🔴 RAII: any throw past this line unwinds the scope and rolls back (§10). The occupant read
    // is INSIDE the transaction so that what gets cleared is what the writes will see.
    atlas::Transaction transaction(connection, ctx);

    const std::array<DbValue, 3> slot_lookup{
        DbValue{static_cast<UInt64>(request.server_id)},
        DbValue{atlas::IdValue(request.character_id)},
        DbValue{static_cast<UInt64>(static_cast<atlas::UInt8>(request.slot))}};
    const std::vector<DbRow> occupants =
        connection.Prepare(kSelectItemsInSlotSql).Query(slot_lookup);

    UInt64 unequipped = 0;
    for (const DbRow& row : occupants) {
        atlas::generated::CharacterItemsRow occupant = CharacterItemsRowFromDb(row);
        if (occupant.item_uid_ == target.item_uid_) {
            // Already wearing it. Re-writing the same value would still be correct, but skipping it
            // keeps "what did this request change" honest in the affected-row counts.
            continue;
        }
        // 🔴 Every occupant, not the first. The uniqueness rule has no constraint behind it, so
        // this code must not assume the state it is meant to be repairing. Two occupants means the
        // invariant was already broken and the recovery is to clear both, loudly.
        if (unequipped != 0) {
            ATLAS_LOG_WARN("slot {} of character {} held more than one item — clearing all",
                           DescribeSlot(request.slot), atlas::IdValue(request.character_id));
        }
        occupant.equip_slot_ = static_cast<atlas::UInt8>(EquipSlot::None);
        connection.Prepare(atlas::generated::kCharacterItemsUpdateByPkSql)
            .Execute(CharacterItemsUpdateParameters(occupant));
        unequipped = occupant.item_uid_;
    }

    target.equip_slot_ = static_cast<atlas::UInt8>(request.slot);
    connection.Prepare(atlas::generated::kCharacterItemsUpdateByPkSql)
        .Execute(CharacterItemsUpdateParameters(target));

    // 🔴 Write 3 touches `characters` in the SAME transaction, and the column it touches is
    // `last_login_at`. That is a compromise and it is stated rather than hidden: §3.3's schema
    // budget is spent (one table, three slots), so there is no `updated_at` and no derived-stat
    // column to write into. What this write exists to prove is that the transaction spans TWO
    // TABLES — a single-row UPDATE is atomic without a transaction and would demonstrate nothing
    // about this layer. When a stat column lands, this write moves to it and nothing else changes.
    character.last_login_at_ = PersistableNow();
    connection.Prepare(atlas::generated::kCharactersUpdateByPkSql)
        .Execute(CharactersUpdateParameters(character));

    if (before_commit) {
        before_commit();
    }

    transaction.Commit();

    ATLAS_LOG_INFO("character {} equipped item {} into slot {} (unequipped {})",
                   atlas::IdValue(request.character_id), request.item_uid,
                   DescribeSlot(request.slot), unequipped);
    return {.result = EquipResult::Ok, .unequipped_item_uid = unequipped};
}

}  // namespace atlas_demo
