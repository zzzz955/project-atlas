#include "game/character_cache.h"

#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/redis/connection.h"
#include "generated/pkt/pkt_codec.h"

namespace atlas_demo {
namespace {

using atlas::Byte;
using atlas::Int32;
using atlas::Int64;
using atlas::SysTime;
using atlas::UInt16;
using atlas::UInt32;
using atlas::UInt64;
using atlas::UInt8;

// DATETIME in this schema has no fractional part, so whole seconds are lossless here — the same
// truncation game/character.cpp already applies before writing one.
Int64 ToEpochSeconds(SysTime value) {
    return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}

SysTime FromEpochSeconds(Int64 seconds) {
    return SysTime{std::chrono::duration_cast<SysTime::duration>(std::chrono::seconds{seconds})};
}

// Redis values are binary safe, but std::string is char-based and Byte is not. The conversion is
// spelled out rather than reinterpret_cast so that no aliasing question is left open.
std::string BytesToString(const std::vector<Byte>& bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (const Byte value : bytes) {
        text.push_back(static_cast<char>(std::to_integer<UInt8>(value)));
    }
    return text;
}

std::vector<Byte> StringToBytes(std::string_view text) {
    std::vector<Byte> bytes;
    bytes.reserve(text.size());
    for (const char value : text) {
        bytes.push_back(static_cast<Byte>(static_cast<UInt8>(value)));
    }
    return bytes;
}

}  // namespace

std::string CharacterCacheKey(atlas::UInt16 server_id, atlas::CharacterId character_id) {
    return std::format("character:{}:{}", server_id, atlas::IdValue(character_id));
}

std::string EncodeCachedCharacter(const CachedCharacter& value) {
    std::vector<Byte> out;
    const atlas::generated::CharactersRow& row = value.character;
    atlas::generated::WriteLe(out, row.server_id_);
    atlas::generated::WriteLe(out, atlas::IdValue(row.character_id_));
    atlas::generated::WriteLe(out, atlas::IdValue(row.account_uid_));
    atlas::generated::WriteUtf8(out, row.name_);
    atlas::generated::WriteLe(out, row.pos_x_);
    atlas::generated::WriteLe(out, row.pos_y_);
    atlas::generated::WriteLe(out, row.level_);
    atlas::generated::WriteLe(out, row.exp_);
    atlas::generated::WriteLe(out, ToEpochSeconds(row.created_at_));
    // The nullable column keeps its presence flag: a cached NULL and a cached epoch-zero are
    // different facts and the database can tell them apart.
    atlas::generated::WriteBool(out, row.last_login_at_.has_value());
    atlas::generated::WriteLe(
        out, row.last_login_at_.has_value() ? ToEpochSeconds(*row.last_login_at_) : Int64{0});

    const UInt16 count = atlas::generated::WriteLength(out, value.items.size());
    for (std::size_t index = 0; index < static_cast<std::size_t>(count); ++index) {
        const Item& entry = value.items[index];
        atlas::generated::WriteLe(out, entry.item_uid);
        atlas::generated::WriteLe(out, entry.item_id);
        atlas::generated::WriteLe(out, entry.stack_count);
        atlas::generated::WriteLe(out, static_cast<UInt8>(entry.slot));
    }
    return BytesToString(out);
}

std::optional<CachedCharacter> DecodeCachedCharacter(std::string_view encoded) {
    const std::vector<Byte> bytes = StringToBytes(encoded);
    std::span<const Byte> cursor(bytes);

    CachedCharacter value;
    atlas::generated::CharactersRow& row = value.character;
    UInt64 character_id = 0;
    UInt64 account_uid = 0;
    Int64 created_at = 0;
    bool has_last_login = false;
    Int64 last_login_at = 0;

    if (!atlas::generated::ReadLe(cursor, row.server_id_) ||
        !atlas::generated::ReadLe(cursor, character_id) ||
        !atlas::generated::ReadLe(cursor, account_uid) ||
        !atlas::generated::ReadUtf8(cursor, row.name_) ||
        !atlas::generated::ReadLe(cursor, row.pos_x_) ||
        !atlas::generated::ReadLe(cursor, row.pos_y_) ||
        !atlas::generated::ReadLe(cursor, row.level_) ||
        !atlas::generated::ReadLe(cursor, row.exp_) ||
        !atlas::generated::ReadLe(cursor, created_at) ||
        !atlas::generated::ReadBool(cursor, has_last_login) ||
        !atlas::generated::ReadLe(cursor, last_login_at)) {
        return std::nullopt;
    }
    row.character_id_ = static_cast<atlas::CharacterId>(character_id);
    row.account_uid_ = static_cast<atlas::AccountId>(account_uid);
    row.created_at_ = FromEpochSeconds(created_at);
    row.last_login_at_ =
        has_last_login ? std::optional<SysTime>{FromEpochSeconds(last_login_at)} : std::nullopt;

    UInt16 count = 0;
    if (!atlas::generated::ReadLe(cursor, count)) {
        return std::nullopt;
    }
    value.items.reserve(static_cast<std::size_t>(count));
    for (std::size_t index = 0; index < static_cast<std::size_t>(count); ++index) {
        Item entry;
        UInt8 slot = 0;
        if (!atlas::generated::ReadLe(cursor, entry.item_uid) ||
            !atlas::generated::ReadLe(cursor, entry.item_id) ||
            !atlas::generated::ReadLe(cursor, entry.stack_count) ||
            !atlas::generated::ReadLe(cursor, slot)) {
            return std::nullopt;
        }
        entry.slot = static_cast<EquipSlot>(slot);
        value.items.push_back(entry);
    }
    return value;
}

void CharacterCache::Get(atlas::Ctx ctx, atlas::UInt16 server_id, atlas::CharacterId character_id,
                         GetHandler handler, atlas::RedisRunner::CompletionPoster poster) {
    atlas::RedisCommand command{.verb = "GET",
                                .args = {CharacterCacheKey(server_id, character_id)}};
    runner_->Submit(
        ctx, command,
        [handler = std::move(handler)](const atlas::Ctx&, const atlas::RedisResult& result) {
            // 🔴 The three failure shapes collapse into one answer on purpose: no connection, a
            // missing key and a value this build cannot read all mean "ask the database".
            if (!result.ok || result.values.empty() || !result.values.front().has_value()) {
                handler(std::nullopt);
                return;
            }
            handler(DecodeCachedCharacter(*result.values.front()));
        },
        std::move(poster));
}

void CharacterCache::Put(atlas::Ctx ctx, atlas::UInt16 server_id, const CachedCharacter& value) {
    atlas::RedisCommand command{
        .verb = "SET",
        .args = {CharacterCacheKey(server_id, value.character.character_id_),
                 EncodeCachedCharacter(value), "EX", std::to_string(kTtlSeconds)}};
    runner_->Submit(ctx, command, [](const atlas::Ctx&, const atlas::RedisResult& result) {
        if (!result.ok) {
            ATLAS_LOG_DEBUG("cache fill skipped: redis did not answer");
        }
    });
}

void CharacterCache::Invalidate(atlas::Ctx ctx, atlas::UInt16 server_id,
                                atlas::CharacterId character_id) {
    atlas::RedisCommand command{.verb = "DEL",
                                .args = {CharacterCacheKey(server_id, character_id)}};
    runner_->Submit(ctx, command, [](const atlas::Ctx&, const atlas::RedisResult& result) {
        if (!result.ok) {
            // 🔴 WARN, not DEBUG, and the asymmetry with Put is the reason: a fill
            // that failed leaves no copy, while an invalidation that failed leaves
            // a WRONG one for up to the TTL. Only one of the two is a lie.
            ATLAS_LOG_WARN(
                "cache invalidation failed: a stale character copy may "
                "be served until it expires");
        }
    });
}

}  // namespace atlas_demo
