#pragma once
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"
#include "atlas/core/types.h"
#include "atlas/redis/redis_runner.h"
#include "game/inventory.h"
#include "generated/db/characters_row.h"

// The demo game's character read cache (architecture-design.md §10.2 — "read cache", the allowed
// row of the table, and nothing below it).
//
// 🔴 READ-THROUGH ON THE READ, INVALIDATE ON THE WRITE — NOT WRITE-THROUGH. The database is the
// source of truth and this is a copy of it. A write-through cache makes the copy authoritative for
// the whole duration of the write, and then a crash in that window leaves two different answers
// with no way to tell which one is the real one. Invalidation cannot produce that state:
// the worst it does is force the next reader to go and ask.
//
// 🔴 REDIS BEING DOWN IS NOT AN OUTAGE OF THIS SERVER. A failed lookup is treated as a miss, and a
// miss goes to the database — the same path a cold key takes. This is the code-level proof that the
// cache is not the source of truth, and server/tests/game_cache_test.cpp pins it against a Redis
// endpoint that is deliberately unreachable.
//
// 🔴 ONE TTL CONSTANT AND NOTHING ELSE. Per-key policies, warming, partial invalidation and
// negative caching are all out of scope (§10.2 keeps the surface at "read cache"), and each of them
// would add a way for the copy to disagree with the truth that no test here would catch.

namespace atlas_demo {

// `character:{server_id}:{character_id}` — the same (server_id, character_id) scope §6 gives the
// primary key, so a key collision across servers is impossible by construction.
[[nodiscard]] std::string CharacterCacheKey(atlas::UInt16 server_id,
                                            atlas::CharacterId character_id);

// What a load produced, minus the outcome: exactly the state a warm load can answer from.
struct CachedCharacter {
    atlas::generated::CharactersRow character{};
    std::vector<Item> items{};
};

// 🔴 The stored value is the game's own little-endian encoding, built with the GENERATED codec
// helpers (generated/pkt/pkt_codec.h), so the byte rules of §8.5 have one implementation in this
// repository rather than two. It is NOT the wire response: the response is a message with a result
// byte and a sequence number, and cacheing that would tie the storage format to the protocol.
[[nodiscard]] std::string EncodeCachedCharacter(const CachedCharacter& value);

// std::nullopt for anything that does not decode. 🔴 A stored value from an older build is not an
// error to report, it is a miss — the DB answer is always available and always right.
[[nodiscard]] std::optional<CachedCharacter> DecodeCachedCharacter(std::string_view encoded);

// The login stamp on the WARM path.
//
// 🔴 The cold path writes the whole row through the generated UPDATE. Writing that statement from a
// cached copy would resurrect every column as it looked when the copy was made, so the warm path
// touches exactly the one column a login owns. Same rule the rest of the game follows
// (architecture-design.md §10): fixed text with `?` placeholders, never assembled at run time.
inline constexpr std::string_view kTouchCharacterLoginSql =
    "UPDATE `characters` SET `last_login_at` = ? WHERE `server_id` = ? AND `character_id` = ?";

class CharacterCache {
public:
    // 🔴 One constant. It is the bound on how stale a warm answer may be, and it is the ONLY
    // policy this class has — see the header note on why there are no others.
    static constexpr atlas::UInt32 kTtlSeconds = 60;

    // std::nullopt is "ask the database": a genuine miss, a decode failure and an unreachable
    // Redis all arrive the same way, because the caller's response to all three is identical.
    using GetHandler = std::function<void(std::optional<CachedCharacter> cached)>;

    explicit CharacterCache(atlas::RedisRunner& runner) : runner_(&runner) {}

    void Get(atlas::Ctx ctx, atlas::UInt16 server_id, atlas::CharacterId character_id,
             GetHandler handler, atlas::RedisRunner::CompletionPoster poster = {});

    // SET with the TTL. Fire and forget: a copy that failed to be written is the state the system
    // was already in a moment earlier.
    void Put(atlas::Ctx ctx, atlas::UInt16 server_id, const CachedCharacter& value);

    // DEL. 🔴 Called AFTER the write committed, for the same reason Record is (see ranking.h): an
    // invalidation issued before the commit can be beaten by a reader that repopulates the key
    // from the pre-commit row, and the stale copy then lives out the full TTL.
    void Invalidate(atlas::Ctx ctx, atlas::UInt16 server_id, atlas::CharacterId character_id);

private:
    // Non-owning observer: the runner outlives this object by construction (cpp-style.md §4.4).
    atlas::RedisRunner* runner_;
};

}  // namespace atlas_demo
