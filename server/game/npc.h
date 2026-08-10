#pragma once
#include <string>
#include <string_view>
#include <utility>

#include "atlas/core/ids.h"
#include "atlas/core/types.h"
#include "game/entity.h"

namespace atlas_demo {

// The other side of the inheritance axis.
//
// 🔴 FIELDS AND A TYPE. NO BEHAVIOUR. The behaviour tree (architecture-design.md §7.3) and the
// server-driven AI that would give an Npc something to do were cut from this slice, and pretending
// otherwise by adding an empty `Tick()` would be worse than leaving it out.
//
// 🔴 The single reason this class exists is that `Entity` with exactly one derived type is a fake
// base class — the virtuals would be dead weight and the "inheritance axis" of the model would be
// a claim with no evidence. Npc makes the dispatch real. Do not grow it past that: an Npc that
// starts acquiring behaviour here is the demo game's minimum set (§3.3) expanding without anyone
// answering which untested core path the addition exercises.
class Npc final : public Entity {
public:
    Npc(atlas::ActorId id, atlas::UInt32 npc_id, atlas::Int32 pos_x, atlas::Int32 pos_y,
        atlas::UInt16 level, std::string name)
        : Entity(id, pos_x, pos_y, level), npc_id_(npc_id), name_(std::move(name)) {}

    [[nodiscard]] EntityKind Kind() const noexcept override { return EntityKind::Npc; }
    [[nodiscard]] std::string_view DisplayName() const noexcept override { return name_; }

    // The static definition this instance was spawned from. 🔴 There is no table behind it in this
    // repository: static data is `info_generator`'s output and that generator lands with the demo
    // CSVs in a later slice (§14). The field is the reference, not a promise that it resolves.
    [[nodiscard]] atlas::UInt32 NpcId() const noexcept { return npc_id_; }

private:
    atlas::UInt32 npc_id_;
    std::string name_;
};

}  // namespace atlas_demo
