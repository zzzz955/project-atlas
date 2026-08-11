#include "game/ranking.h"

#include <charconv>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"
#include "atlas/core/types.h"
#include "atlas/redis/connection.h"

namespace atlas_demo {
namespace {

using atlas::UInt16;
using atlas::UInt64;

// A ZSET score is a double, so an exp above 2^53 would not round trip exactly.
//
// 🔴 That narrowing is real and it is accepted rather than hidden: `characters.exp` is UInt64 and
// this projection is not. It is safe here for one reason only — the database, not the sorted set,
// is the value anybody is ever paid out of (see the header). A design that made the ranking
// authoritative would have to store the exact value somewhere else first.
UInt64 ParseScore(std::string_view text) {
    double score = 0.0;
    const std::from_chars_result parsed =
        std::from_chars(text.data(), text.data() + text.size(), score);
    if (parsed.ec != std::errc{} || score <= 0.0) {
        return 0;
    }
    return static_cast<UInt64>(score);
}

UInt64 ParseMember(std::string_view text) {
    UInt64 value = 0;
    const std::from_chars_result parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return 0;
    }
    return value;
}

}  // namespace

std::string ExpRankingKey(atlas::UInt16 server_id) {
    return std::format("ranking:{}:exp", server_id);
}

void ExpRanking::Record(atlas::Ctx ctx, atlas::UInt16 server_id, atlas::CharacterId character_id,
                        atlas::UInt64 exp) {
    atlas::RedisCommand command{.verb = "ZADD",
                                .args = {ExpRankingKey(server_id), std::to_string(exp),
                                         std::to_string(atlas::IdValue(character_id))}};
    runner_->Submit(ctx, command, [](const atlas::Ctx&, const atlas::RedisResult& result) {
        if (!result.ok) {
            // DEBUG, not WARN: with the cache down this fires on every load, and a
            // lost copy is not an incident. The outage itself is already reported
            // once by the connection's own logger.
            ATLAS_LOG_DEBUG("ranking write skipped: redis did not answer");
        }
    });
}

void ExpRanking::Top(atlas::Ctx ctx, atlas::UInt16 server_id, atlas::UInt16 count,
                     TopHandler handler, atlas::RedisRunner::CompletionPoster poster) {
    const UInt16 clamped = count > kMaxRankEntries ? kMaxRankEntries : count;
    if (clamped == 0) {
        handler(true, std::vector<RankEntry>{});
        return;
    }

    // 🔴 ZREVRANGE's stop index is INCLUSIVE, so the last index is count - 1. The widening is
    // explicit because `clamped - 1` would promote to `int` and cpp-style.md §4.1 bans that type
    // from this layer; `clamped` is at least 1 here, so the subtraction cannot wrap.
    const atlas::UInt32 last_index = static_cast<atlas::UInt32>(clamped) - 1U;
    atlas::RedisCommand command{
        .verb = "ZREVRANGE",
        .args = {ExpRankingKey(server_id), "0", std::to_string(last_index), "WITHSCORES"}};

    runner_->Submit(
        ctx, command,
        [handler = std::move(handler)](const atlas::Ctx&, const atlas::RedisResult& result) {
            std::vector<RankEntry> entries;
            if (!result.ok) {
                handler(false, entries);
                return;
            }
            // WITHSCORES flattens to member, score, member, score ... A trailing half pair would
            // mean the reply was truncated, so the odd element is dropped rather than guessed at.
            entries.reserve(result.values.size() / 2U);
            for (std::size_t index = 0; index + 1U < result.values.size(); index += 2U) {
                const std::optional<std::string>& member = result.values[index];
                const std::optional<std::string>& score = result.values[index + 1U];
                if (!member.has_value() || !score.has_value()) {
                    continue;
                }
                entries.push_back(
                    RankEntry{.character_id = static_cast<atlas::CharacterId>(ParseMember(*member)),
                              .exp = ParseScore(*score)});
            }
            handler(true, entries);
        },
        std::move(poster));
}

}  // namespace atlas_demo
