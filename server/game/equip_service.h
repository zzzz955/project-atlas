#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "game/inventory.h"

namespace atlas_demo {

// Why an equip request was refused. 🔴 Expected failures, so they are a return value and not an
// exception (architecture-design.md §11.2a). What DOES throw here is the database refusing a
// statement, and that unwinds `Transaction` and rolls the whole thing back — which is the property
// the fault-injection test exists to observe.
enum class EquipResult : atlas::UInt8 {
    Ok = 0,
    InvalidSlot,
    ItemNotFound,
    NotOwned,  // 🔴 §8.2 layer 3, server authority: the item exists, but not for this character
    CharacterNotFound,
    // 🔴 §8.2 layer 3 again, now against the STATIC data (generated/info/item_info.h). The row is
    // real and it is yours; what the server rejects is the claim about what the item IS. Appended
    // rather than inserted: this enumerator is the response byte on the wire (§8.5).
    UnknownItem,   // no such item_id in shared/datas/item.csv
    SlotMismatch,  // the item exists, but not for the slot the request names
};

[[nodiscard]] std::string_view DescribeEquipResult(EquipResult result) noexcept;

// Equipping one item, atomically.
//
// 🔴 THIS IS THE DATABASE HIGHLIGHT OF THE SLICE, AND THE REASON IS THE MISSING CONSTRAINT.
// "At most one item per equip slot" cannot be declared to MySQL: there is no partial unique index,
// so a UNIQUE restricted to `equip_slot != 0` does not exist (server/db/schema.json says the same).
// The invariant therefore has exactly two defenders — the transaction below, and the tests that try
// to break it. A single-row UPDATE would have proven nothing: it is atomic without a transaction.
// Three writes across two tables is what makes the rollback observable.
//
//     1. clear the slot          UPDATE character_items SET equip_slot = 0  (the current occupant)
//     2. take the slot           UPDATE character_items SET equip_slot = N  (the target item)
//     3. touch the character     UPDATE characters       SET last_login_at  (see the note in .cpp)
//
// 🔴 PER-CHARACTER SERIALISATION — WHY A LOCK AND NOT A STRAND.
// Two concurrent equip requests for the SAME character interleave their read-then-write and both
// can conclude the slot was free; the invariant then breaks with no error anywhere. The two
// available answers were §10's per-character lock and pinning the character to a strand, and the
// choice is forced by WHERE this code runs: the DB client is synchronous, so §9 puts it on the DB
// thread pool, deliberately NOT on an io_context worker. A strand only serialises work executing on
// its io_context — pinning a character to one would mean running the blocking query on an I/O
// thread, which freezes every session that thread serves. That is the exact failure the separate
// pool exists to prevent, so strand pinning is not available at this layer and the lock is.
//
// 🔴 This does not contradict §9.1's "concurrency is strands, single model". §9.1 permits a lock on
// a genuinely shared resource, and a character's rows are shared by N unrelated DB threads — the
// same line connection_pool.h sits on. The lock is taken and released entirely inside one call on
// one DB thread; it never spans an async boundary, so no handler can ever be waiting on it.
class EquipService {
public:
    struct Request {
        atlas::UInt16 server_id{0};
        // 🔴 The SESSION's character, never a value out of the packet. Everything after the login
        // derives its identity from the connection, which is the half of server authority (§8.2
        // layer 3) that exists before auth (§12) lands.
        atlas::CharacterId character_id{};
        atlas::UInt64 item_uid{0};
        EquipSlot slot{EquipSlot::None};
    };

    struct Outcome {
        EquipResult result{EquipResult::Ok};
        // The item that left the slot, or 0 when it was empty.
        atlas::UInt64 unequipped_item_uid{0};
    };

    // 🔴 A fault-injection seam, and it is in the production signature ON PURPOSE. The property
    // this class exists to defend — "a failure at the third write leaves the invariant untouched" —
    // is not observable from outside unless that write can be made to fail on demand. Reproducing
    // it by other means (killing the connection, corrupting a row) would test the driver instead of
    // this transaction. It is empty in production; the only caller that passes one is the test.
    using FaultInjector = std::function<void()>;

    EquipService() = default;

    EquipService(const EquipService&) = delete;
    EquipService& operator=(const EquipService&) = delete;
    EquipService(EquipService&&) = delete;
    EquipService& operator=(EquipService&&) = delete;

    // Runs on a DB thread with a leased connection. `ctx` is the job's own ledger — the transaction
    // scope writes tx_state into it so the caller can still ask "was one open?" after the guard
    // returned (§10.3).
    Outcome Equip(atlas::Ctx& ctx, atlas::Connection& connection, const Request& request,
                  const FaultInjector& before_commit = {});

private:
    using CharacterKey = std::pair<atlas::UInt16, atlas::UInt64>;

    std::mutex& LockFor(const CharacterKey& key);

    // 🔴 Entries are never removed. The map is bounded by the number of distinct characters this
    // process has served, and erasing one would mean proving nobody is about to take it — the
    // classic way a lock registry grows a use-after-free. A GAME server that outlives its memory
    // budget here has a different problem: the durable answer §10.2 already names is a distributed
    // lock, not a smaller map.
    std::mutex registry_mutex_;
    std::map<CharacterKey, std::unique_ptr<std::mutex>> character_locks_;
};

}  // namespace atlas_demo
