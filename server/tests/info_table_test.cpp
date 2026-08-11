#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>

#include "atlas/core/types.h"
// 🔴 The AGGREGATE header, on purpose. It is generator output too (info_all.h), and "every static
// table is reachable from one include" is only true if something actually includes it that way.
#include "generated/info/info_all.h"

// The generated static-data tables (architecture-design.md §14 — info_generator).
//
// 🔴 WHAT THIS SUITE CLAIMS.
//   * The primary-key lookup answers with the row the CSV declares, and answers nullptr — not a
//     nearby row, and not an exception — for an id the CSV does not declare. That null is what
//     §8.2 layer 3 rests on in the equip path.
//   * The rows really are emitted in ascending key order, which is what makes the lookup a binary
//     search rather than a scan.
//   * A `C` column is NOT in the server struct. The target filter is the seam
//     (`[data-gen].server_targets`), and a filter nobody checks is a filter that quietly stops.
//
// 🔴 The expected values below are transcribed BY HAND from shared/datas/item.csv. Deriving them
// from the generator would only prove the generator agrees with itself — the same reason
// server/generated/db/tests/db_schema_test.cpp keeps its own copy of the schema.

namespace {

using atlas::UInt32;
using atlas::UInt8;
using atlas::generated::FindItemInfo;
using atlas::generated::ItemInfoRow;
using atlas::generated::ItemInfoRows;

// shared/datas/item.csv, row 5 onward. slot: 1 = weapon, 2 = armor, 3 = trinket (§3.3).
struct ExpectedItem {
    UInt32 id;
    UInt8 slot;
};

constexpr std::array<ExpectedItem, 4> kExpected = {{
    {.id = 1001, .slot = 1},
    {.id = 1002, .slot = 1},
    {.id = 2001, .slot = 2},
    {.id = 3001, .slot = 3},
}};

TEST(InfoTableTest, EveryRowOfTheCsvIsPresentAndFoundByItsKey) {
    ASSERT_EQ(ItemInfoRows().size(), kExpected.size());
    ASSERT_EQ(atlas::generated::kItemInfoRowCount, kExpected.size());

    for (const ExpectedItem& expected : kExpected) {
        const ItemInfoRow* found = FindItemInfo(expected.id);
        ASSERT_NE(found, nullptr) << "item_id " << expected.id << " is missing";
        EXPECT_EQ(found->id_, expected.id);
        EXPECT_EQ(found->slot_, expected.slot);
    }
}

// 🔴 The case the equip path depends on. A client naming an id nobody defined must produce a
// refusal, and a lookup that returned a neighbouring row (or threw) could not produce one.
TEST(InfoTableTest, AnIdTheCsvDoesNotDeclareIsNullAndNotANeighbour) {
    EXPECT_EQ(FindItemInfo(0), nullptr);
    EXPECT_EQ(FindItemInfo(1000), nullptr);  // just below the first key
    EXPECT_EQ(FindItemInfo(1500), nullptr);  // between two keys
    EXPECT_EQ(FindItemInfo(9999), nullptr);  // past the last key
}

// The lookup is a binary search, so ordering is not cosmetic — an unsorted table would answer
// nullptr for rows that are right there.
TEST(InfoTableTest, RowsAreEmittedInAscendingKeyOrder) {
    const std::span<const ItemInfoRow> rows = ItemInfoRows();
    EXPECT_TRUE(
        std::is_sorted(rows.begin(), rows.end(),
                       [](const ItemInfoRow& a, const ItemInfoRow& b) { return a.id_ < b.id_; }));
}

// 🔴 THE SEAM, ASSERTED. item.csv has three columns and the server keeps two: `name` is marked `C`
// and must not exist here. If a future change lets client-only columns through, this stops
// compiling — which is the only kind of documentation that survives.
TEST(InfoTableTest, ClientOnlyColumnsAreNotInTheServerStruct) {
    EXPECT_EQ(atlas::generated::kItemInfoColumnCount, std::size_t{2});
    ASSERT_EQ(atlas::generated::kItemInfoColumns.size(), std::size_t{2});
    EXPECT_EQ(atlas::generated::kItemInfoColumns[0].name_, "id");
    EXPECT_TRUE(atlas::generated::kItemInfoColumns[0].primary_key_);
    EXPECT_EQ(atlas::generated::kItemInfoColumns[1].name_, "slot");

    for (const atlas::generated::InfoColumnMeta& column : atlas::generated::kItemInfoColumns) {
        EXPECT_NE(column.target_, "C") << column.name_ << " is client-only and must not be here";
    }

    // Fixed width, not `int` (cpp-style.md §4.1). types.json has no `int` key at all, so the CSV
    // could not have asked for one — this pins the result of that.
    static_assert(std::is_same_v<decltype(ItemInfoRow::id_), UInt32>);
    static_assert(std::is_same_v<decltype(ItemInfoRow::slot_), UInt8>);
}

}  // namespace
