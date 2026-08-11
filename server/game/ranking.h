#pragma once
#include <functional>
#include <string>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"
#include "atlas/core/types.h"
#include "atlas/redis/redis_runner.h"

// The demo game's exp ranking (architecture-design.md §10.2 — "read cache (global settings ·
// ranking snapshot)", the first row of the allow table).
//
// 🔴 THIS FILE COULD NOT BE WRITTEN UNDER server/atlas/**. `ranking` is on
// tools/core_purity/denylist.txt, so the gate (§15.4) fails the build if the core learns this word.
// That is deliberate and it is the point of the split: `atlas/redis` executes commands, and the KEY
// FORMAT below — what a ranking is named, what it is scored by, how many members it holds — is game
// vocabulary. A core that owned the key string would own the game's data model.
//
// 🔴 THE SOURCE IS `characters.exp`, WHICH ALREADY EXISTS. No new column and no new table: the demo
// minimum set (§3.3) is not widened for a leaderboard, so the ranking is a projection of a column
// the game already persists.
//
// 🔴 THE DATABASE IS THE SOURCE OF TRUTH AND THIS IS A COPY. Every write goes in after the
// transaction that produced the value COMMITTED (see handlers.h), and a failed write is a lost copy
// rather than a lost fact. Rebuilding the whole ranking is a `SELECT` away, which is precisely why
// losing it is allowed to be cheap.

namespace atlas_demo {

// `ranking:{server_id}:exp` — one sorted set per server, because §6 scopes a character by
// (server_id, character_id) and a ranking that merged two servers would be comparing populations
// that never meet.
[[nodiscard]] std::string ExpRankingKey(atlas::UInt16 server_id);

struct RankEntry {
    atlas::CharacterId character_id{};
    atlas::UInt64 exp{0};

    friend bool operator==(const RankEntry&, const RankEntry&) = default;
};

// 🔴 A ceiling on what one request may ask for. §8.1 caps a payload at 16 KiB and an unbounded
// ZREVRANGE would let a client choose both the server's work and the response size.
inline constexpr atlas::UInt16 kMaxRankEntries = 100;

class ExpRanking {
public:
    // `ok` is false when Redis did not answer. 🔴 It is NOT "the ranking is empty" — a caller that
    // conflated the two would report an outage as a leaderboard nobody is on.
    using TopHandler = std::function<void(bool ok, const std::vector<RankEntry>& entries)>;

    explicit ExpRanking(atlas::RedisRunner& runner) : runner_(&runner) {}

    // ZADD. 🔴 CALL ONLY AFTER THE TRANSACTION THAT READ THIS VALUE COMMITTED. Writing the copy
    // first would record a number the database may still refuse, and the ranking has no way to
    // take it back.
    //
    // Fire and forget: there is no completion because there is no decision to make. A failure is
    // logged at DEBUG and the next load of that character puts the value back.
    void Record(atlas::Ctx ctx, atlas::UInt16 server_id, atlas::CharacterId character_id,
                atlas::UInt64 exp);

    // ZREVRANGE ... WITHSCORES, highest exp first. `count` is clamped to kMaxRankEntries.
    void Top(atlas::Ctx ctx, atlas::UInt16 server_id, atlas::UInt16 count, TopHandler handler,
             atlas::RedisRunner::CompletionPoster poster = {});

private:
    // Non-owning observer: the runner outlives this object by construction (cpp-style.md §4.4).
    atlas::RedisRunner* runner_;
};

}  // namespace atlas_demo
