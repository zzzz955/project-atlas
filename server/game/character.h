#pragma once
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

namespace atlas_demo {

// 🔴 OWNERSHIP, NOT INHERITANCE — see the two-axes note at the top of entity.h.
//
// An account holds N characters; a character is not a kind of account. The relationship is a list
// of ids plus an `Owns()` question, which is all §6 asks for: the account itself is owned by
// `platform-auth` (§12) and this server never stores one. That the type is this small is the
// evidence that the relation was modelled as ownership — an inheritance answer would have had to
// duplicate account state into every character.
class Account {
public:
    explicit Account(atlas::AccountId uid) noexcept : uid_(uid) {}

    [[nodiscard]] atlas::AccountId Uid() const noexcept { return uid_; }
    [[nodiscard]] const std::vector<atlas::CharacterId>& Characters() const noexcept {
        return characters_;
    }

    void Own(atlas::CharacterId character_id) {
        if (!Owns(character_id)) {
            characters_.push_back(character_id);
        }
    }

    [[nodiscard]] bool Owns(atlas::CharacterId character_id) const noexcept {
        return std::find(characters_.begin(), characters_.end(), character_id) != characters_.end();
    }

private:
    atlas::AccountId uid_;
    // 🔴 The ids, not the objects. A character is loaded from GAME's database when it is played;
    // holding the instances here would make "log in" mean "materialise every character the account
    // ever made", and §10 forbids relation auto-loading for exactly that reason.
    std::vector<atlas::CharacterId> characters_;
};

// A player character: the persisted half of §6's third identity tier.
//
// 🔴 The primary key is `(server_id, character_id)` (§6), so `server_id` travels with the object.
// It is not a strong-typed id — core/ids.h says why: nothing passes it as a bare argument next to
// another 16-bit value, and a type nobody can swap by mistake buys nothing.
class Character final : public Entity {
public:
    Character(atlas::UInt16 server_id, atlas::CharacterId character_id,
              atlas::AccountId account_uid, std::string name, atlas::Int32 pos_x,
              atlas::Int32 pos_y, atlas::UInt16 level, atlas::UInt64 exp);

    [[nodiscard]] EntityKind Kind() const noexcept override { return EntityKind::Character; }
    [[nodiscard]] std::string_view DisplayName() const noexcept override { return name_; }

    [[nodiscard]] atlas::UInt16 ServerId() const noexcept { return server_id_; }
    [[nodiscard]] atlas::CharacterId CharacterKey() const noexcept { return character_id_; }
    [[nodiscard]] atlas::AccountId AccountUid() const noexcept { return account_uid_; }
    [[nodiscard]] atlas::UInt64 Exp() const noexcept { return exp_; }
    void SetExp(atlas::UInt64 exp) noexcept { exp_ = exp; }

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

// ── The persistence edge ─────────────────────────────────────────────────────────────────────
// The same seam inventory.h describes: atlas/db knows no table (§10.3), so the code that knows both
// the generated row and the runtime lives here.

// The wall-clock value this schema can actually store: DATETIME has no fractional part, so a
// timestamp carrying microseconds would not survive the round trip and every comparison against a
// re-read row would fail. 🔴 SysClock, not Clock — §4.2 of cpp-style.md: the monotonic clock is for
// timeouts and ticks, the wall clock is for storage.
[[nodiscard]] atlas::SysTime PersistableNow();

[[nodiscard]] Character CharacterFromRow(const atlas::generated::CharactersRow& row);

[[nodiscard]] atlas::generated::CharactersRow CharactersRowFromDb(const atlas::DbRow& row);

// Binding parameters for `kCharactersUpdateByPkSql`, in the generated binding order.
[[nodiscard]] std::vector<atlas::DbValue> CharactersUpdateParameters(
    const atlas::generated::CharactersRow& row);

}  // namespace atlas_demo
