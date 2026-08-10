#pragma once
#include <string_view>

#include "atlas/core/ids.h"
#include "atlas/core/types.h"

// The demo game's domain model (architecture-design.md §3.3 · §7).
//
// 🔴 THIS DIRECTORY IS OUTSIDE server/atlas/**, AND THAT IS THE POINT — NOT A LAYOUT PREFERENCE.
// The core-purity gate (§15.4) fails the build if the words `item`, `inventory` or `equip` appear
// anywhere under server/atlas/**, so a game that has an inventory structurally cannot be written
// into the framework. `server/game/` is where the demo game lives; the core links nothing from here
// and never learns this game exists (§14, core / contract / game). Swapping the game means
// replacing this directory plus the generator inputs — not editing the core.
//
// 🔴 TWO AXES. CONFUSING THEM IS THE MISTAKE THIS COMMENT EXISTS TO PREVENT.
//
//     OWNERSHIP     Account (1) ── owns ── Character (N)      🔴 NOT inheritance
//     INHERITANCE   Entity → Character / Npc                  virtual dispatch lives here
//
// A `Character : Account` hierarchy cannot express "the second character of one account": the
// account state would be duplicated once per character, and §6's identity ladder
// (account → server → character) says one account holds many characters across many servers.
// Ownership is a field and a lookup, never a base class. The inheritance axis is a different
// question — "what is this thing in the world" — and that one really is is-a.

namespace atlas_demo {

// Which concrete kind an Entity is, for the places that must branch without a dynamic_cast.
enum class EntityKind : atlas::UInt8 { Character, Npc };

// Anything that exists in the world: an identity, a position and a level.
//
// 🔴 The base is deliberately thin. §7's Actor (AoI registration, tick membership, behaviour) is a
// WORLD-server concern and this slice builds GAME, so the fields here are the ones GAME persists
// and nothing more. Growing this class is how the demo game's minimum set (§3.3) gets expanded by
// accident.
class Entity {
public:
    virtual ~Entity() = default;

    Entity(const Entity&) = delete;
    Entity& operator=(const Entity&) = delete;
    Entity(Entity&&) = delete;
    Entity& operator=(Entity&&) = delete;

    [[nodiscard]] atlas::ActorId Id() const noexcept { return id_; }
    [[nodiscard]] atlas::Int32 PosX() const noexcept { return pos_x_; }
    [[nodiscard]] atlas::Int32 PosY() const noexcept { return pos_y_; }
    [[nodiscard]] atlas::UInt16 Level() const noexcept { return level_; }

    void SetPosition(atlas::Int32 pos_x, atlas::Int32 pos_y) noexcept {
        pos_x_ = pos_x;
        pos_y_ = pos_y;
    }

    void SetLevel(atlas::UInt16 level) noexcept { level_ = level; }

    // The two things every entity answers differently. Without a second derived type these would be
    // dead virtuals — see the note on Npc.
    [[nodiscard]] virtual EntityKind Kind() const noexcept = 0;
    [[nodiscard]] virtual std::string_view DisplayName() const noexcept = 0;

protected:
    Entity(atlas::ActorId id, atlas::Int32 pos_x, atlas::Int32 pos_y, atlas::UInt16 level) noexcept
        : id_(id), pos_x_(pos_x), pos_y_(pos_y), level_(level) {}

private:
    atlas::ActorId id_;
    atlas::Int32 pos_x_;
    atlas::Int32 pos_y_;
    atlas::UInt16 level_;
};

}  // namespace atlas_demo
