// 🔴 HAND-WRITTEN. This is the only .cpp under server/generated/db/ that is not generator output —
//    it is the gate ON the generator. It lives here rather than in server/tests/ so that the
//    db_generator work item owns one directory end to end and never collides with the core tests.
//
// What it proves (architecture-design.md §6 / §10, cpp-style.md §4.1 / §4.3):
//   1. Schema vs struct   — the column list below is transcribed BY HAND from server/db/schema.json.
//                           Comparing it against the generated metadata is the cross-check: a column
//                           renamed, reordered, retyped or dropped in the schema fails here until a
//                           human confirms the change. A test generated from the same input would
//                           only prove the generator agrees with itself.
//   2. Fixed width        — every fixed-width column is static_assert-ed at its declared size, so a
//                           row can never change shape between Windows (LLP64) and Linux (LP64).
//   3. Prepared statement — the `?` count of every statement equals the length of its binding array,
//                           and the key columns of UPDATE bind last.
//   4. Strong-typed ID    — character_id is CharacterId, and no implicit conversion exists in either
//                           direction, so a swapped argument is a compile error and not a silently
//                           wrong row.
//
// 🔴 What is NOT here, on purpose: anything that executes SQL. This slice emits row structs,
//    metadata and statement text only — connection, transaction scope and per-character lock are
//    the ORM runtime's job in a later slice (architecture-design.md §10).

#include <array>
#include <cstddef>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

#include "atlas/core/ids.h"
#include "atlas/core/types.h"
#include "generated/db/characters_row.h"
#include "generated/db/db_meta.h"

namespace {

using atlas::AccountId;
using atlas::CharacterId;
using atlas::IdValue;
using atlas::Int32;
using atlas::UInt16;
using atlas::UInt64;
using atlas::generated::ColumnMeta;
using atlas::generated::ColumnType;
using atlas::generated::CountPlaceholders;

// ── 1. Schema vs struct ─────────────────────────────────────────────────────────────────────────
// 🔴 Transcribed by hand from server/db/schema.json. Do not regenerate it, and do not "fix" it to
//    make a failure go away — a mismatch means the schema moved and the row layout moved with it.
constexpr std::array<ColumnMeta, 10> kExpectedCharacterColumns = {{
    {.name_ = "server_id", .type_ = ColumnType::UInt16, .primary_key_ = true, .nullable_ = false, .max_length_ = 0},
    {.name_ = "character_id", .type_ = ColumnType::UInt64, .primary_key_ = true, .nullable_ = false, .max_length_ = 0},
    {.name_ = "account_uid", .type_ = ColumnType::UInt64, .primary_key_ = false, .nullable_ = false, .max_length_ = 0},
    {.name_ = "name", .type_ = ColumnType::String, .primary_key_ = false, .nullable_ = false, .max_length_ = 32},
    {.name_ = "pos_x", .type_ = ColumnType::Int32, .primary_key_ = false, .nullable_ = false, .max_length_ = 0},
    {.name_ = "pos_y", .type_ = ColumnType::Int32, .primary_key_ = false, .nullable_ = false, .max_length_ = 0},
    {.name_ = "level", .type_ = ColumnType::UInt16, .primary_key_ = false, .nullable_ = false, .max_length_ = 0},
    {.name_ = "exp", .type_ = ColumnType::UInt64, .primary_key_ = false, .nullable_ = false, .max_length_ = 0},
    {.name_ = "created_at", .type_ = ColumnType::DateTime, .primary_key_ = false, .nullable_ = false, .max_length_ = 0},
    {.name_ = "last_login_at", .type_ = ColumnType::DateTime, .primary_key_ = false, .nullable_ = true, .max_length_ = 0},
}};

TEST(DbSchema, MetadataMatchesTheSchemaColumnByColumn) {
    ASSERT_EQ(atlas::generated::kCharactersColumnCount, kExpectedCharacterColumns.size());
    ASSERT_EQ(atlas::generated::kCharactersColumns.size(), kExpectedCharacterColumns.size());
    EXPECT_EQ(atlas::generated::kCharactersTable, "characters");

    for (std::size_t i = 0; i < kExpectedCharacterColumns.size(); ++i) {
        const ColumnMeta& want = kExpectedCharacterColumns[i];
        const ColumnMeta& got = atlas::generated::kCharactersColumns[i];
        // Reported per column: a whole-array comparison would only say "the table changed".
        EXPECT_EQ(got.name_, want.name_) << "column " << i;
        EXPECT_EQ(got.type_, want.type_) << "column " << want.name_;
        EXPECT_EQ(got.primary_key_, want.primary_key_) << "column " << want.name_;
        EXPECT_EQ(got.nullable_, want.nullable_) << "column " << want.name_;
        EXPECT_EQ(got.max_length_, want.max_length_) << "column " << want.name_;
    }
}

TEST(DbSchema, PrimaryKeyIsTheServerScopedCharacterPair) {
    // architecture-design.md §6 — one account owns characters on several servers, so the key is the
    // pair and never character_id alone.
    std::size_t key_columns = 0;
    for (const ColumnMeta& column : atlas::generated::kCharactersColumns) {
        if (column.primary_key_) ++key_columns;
    }
    EXPECT_EQ(key_columns, 2U);
    EXPECT_TRUE(atlas::generated::kCharactersColumns[atlas::generated::kCharactersColServerId].primary_key_);
    EXPECT_TRUE(atlas::generated::kCharactersColumns[atlas::generated::kCharactersColCharacterId].primary_key_);
}

TEST(DbSchema, RowFieldsCoverEveryColumn) {
    // Naming every field keeps the struct and the schema coupled at COMPILE time: a renamed or
    // removed column stops this file from building, which is louder than a failed assertion.
    const atlas::generated::CharactersRow row{
        .server_id_ = UInt16{7},
        .character_id_ = CharacterId{42},
        .account_uid_ = AccountId{1234567890123456U},
        .name_ = "atlas",
        .pos_x_ = Int32{-3},
        .pos_y_ = Int32{9},
        .level_ = UInt16{5},
        .exp_ = UInt64{999},
        .created_at_ = {},
        .last_login_at_ = {},
    };

    EXPECT_EQ(IdValue(row.character_id_), 42U);
    EXPECT_EQ(row.name_.size(), 5U);
    EXPECT_LE(row.name_.size(), atlas::generated::kCharactersNameMaxLength);
    EXPECT_FALSE(row.last_login_at_.has_value()) << "a nullable column defaults to absent, not to 0";

    // Defaults come from the schema, so the two cannot drift apart.
    const atlas::generated::CharactersRow fresh{};
    EXPECT_EQ(fresh.level_, 1U);
    EXPECT_EQ(fresh.exp_, 0U);
    EXPECT_EQ(fresh.pos_x_, 0);
    EXPECT_EQ(fresh.pos_y_, 0);
}

// ── 2. Fixed width ──────────────────────────────────────────────────────────────────────────────
// cpp-style.md §4.1 — Windows is LLP64 and Linux is LP64. The generator emits its own width
// assertions next to the struct; these repeat the load-bearing ones so the reason lives with the
// test that would otherwise be blamed for a size change.
static_assert(sizeof(atlas::generated::CharactersRow::character_id_) == 8U,
              "characters.character_id is a 64-bit identifier on every platform");
static_assert(sizeof(atlas::generated::CharactersRow::exp_) == 8U,
              "characters.exp is a 64-bit counter on every platform");
static_assert(sizeof(atlas::generated::CharactersRow::pos_x_) == 4U,
              "characters.pos_x is a 32-bit grid coordinate on every platform");
static_assert(sizeof(atlas::generated::CharactersRow::server_id_) == 2U,
              "characters.server_id is a 16-bit scope on every platform");

// ── 3. Prepared statements ──────────────────────────────────────────────────────────────────────
TEST(DbSchema, EveryStatementBindsExactlyItsPlaceholders) {
    EXPECT_EQ(CountPlaceholders(atlas::generated::kCharactersSelectByPkSql),
              atlas::generated::kCharactersSelectByPkBinding.size());
    EXPECT_EQ(CountPlaceholders(atlas::generated::kCharactersInsertSql),
              atlas::generated::kCharactersInsertBinding.size());
    EXPECT_EQ(CountPlaceholders(atlas::generated::kCharactersUpdateByPkSql),
              atlas::generated::kCharactersUpdateByPkBinding.size());
    EXPECT_EQ(CountPlaceholders(atlas::generated::kCharactersDeleteByPkSql),
              atlas::generated::kCharactersDeleteByPkBinding.size());
}

TEST(DbSchema, BindingIndicesStayInsideTheColumnTable) {
    const auto in_range = [](std::size_t index) {
        return index < atlas::generated::kCharactersColumnCount;
    };
    for (const std::size_t index : atlas::generated::kCharactersInsertBinding) EXPECT_TRUE(in_range(index));
    for (const std::size_t index : atlas::generated::kCharactersUpdateByPkBinding) EXPECT_TRUE(in_range(index));
    for (const std::size_t index : atlas::generated::kCharactersSelectByPkBinding) EXPECT_TRUE(in_range(index));
    for (const std::size_t index : atlas::generated::kCharactersDeleteByPkBinding) EXPECT_TRUE(in_range(index));
}

TEST(DbSchema, UpdateBindsTheKeyColumnsLast) {
    // 🔴 The WHERE clause comes after the SET list, so the key parameters are the last two. Binding
    // them first would update the wrong row while every statement still "looked" correct.
    const auto& binding = atlas::generated::kCharactersUpdateByPkBinding;
    ASSERT_EQ(binding.size(), atlas::generated::kCharactersColumnCount);
    EXPECT_EQ(binding[binding.size() - 2], atlas::generated::kCharactersColServerId);
    EXPECT_EQ(binding[binding.size() - 1], atlas::generated::kCharactersColCharacterId);
}

TEST(DbSchema, StatementsAreFixedTextWithPlaceholders) {
    // 🔴 architecture-design.md §10 — no SQL assembly. Values only ever reach the database through a
    // placeholder, so a statement must carry `?` and must never carry a quoted literal.
    const std::array<std::string_view, 4> statements = {{
        atlas::generated::kCharactersSelectByPkSql,
        atlas::generated::kCharactersInsertSql,
        atlas::generated::kCharactersUpdateByPkSql,
        atlas::generated::kCharactersDeleteByPkSql,
    }};
    for (const std::string_view sql : statements) {
        EXPECT_GT(CountPlaceholders(sql), 0U) << sql;
        EXPECT_EQ(sql.find('\''), std::string_view::npos) << "quoted literal in " << sql;
    }
}

TEST(DbSchema, SelectProjectsEveryColumnInMetadataOrder) {
    // The row struct is filled from the result set positionally, so the projection order and the
    // metadata order have to be the same list.
    const std::string_view sql = atlas::generated::kCharactersSelectByPkSql;
    std::size_t cursor = 0;
    for (const ColumnMeta& column : atlas::generated::kCharactersColumns) {
        const std::size_t found = sql.find(column.name_, cursor);
        ASSERT_NE(found, std::string_view::npos) << "column missing from projection: " << column.name_;
        cursor = found + column.name_.size();
    }
}

// ── 4. Strong-typed IDs ─────────────────────────────────────────────────────────────────────────
// cpp-style.md §4.3 — the whole value of the rule is the conversions that do NOT exist. If any of
// these flipped, Load(server_id, character_id) would keep compiling with the arguments swapped.
static_assert(std::is_same_v<decltype(atlas::generated::CharactersRow::character_id_), CharacterId>,
              "characters.character_id must be the strong-typed CharacterId");
static_assert(std::is_same_v<decltype(atlas::generated::CharactersRow::account_uid_), AccountId>,
              "characters.account_uid must be the strong-typed AccountId");
static_assert(!std::is_convertible_v<CharacterId, UInt64>,
              "CharacterId must not decay to its underlying value implicitly");
static_assert(!std::is_convertible_v<UInt64, CharacterId>,
              "a raw integer must not become a CharacterId implicitly");
static_assert(!std::is_convertible_v<CharacterId, AccountId>,
              "two identifiers of the same width must stay distinct types");
static_assert(!std::is_same_v<decltype(atlas::generated::CharactersRow::server_id_), CharacterId>,
              "server_id is deliberately still a plain width (see atlas/core/ids.h)");

TEST(DbSchema, IdentifierValueSurvivesTheStrongType) {
    // The one explicit way back to the raw value; the column itself is stored as UInt64.
    const CharacterId id{0xFFFF'FFFF'FFFF'FFFFU};
    EXPECT_EQ(IdValue(id), 0xFFFF'FFFF'FFFF'FFFFU);
    EXPECT_EQ(atlas::generated::kCharactersColumns[atlas::generated::kCharactersColCharacterId].type_,
              ColumnType::UInt64);
}

}  // namespace
