#include <gtest/gtest.h>

#include <cstddef>
#include <type_traits>

#include "atlas/core/time.h"
#include "atlas/core/types.h"

namespace {

// cpp-style.md §4.3 — the strong-typed ID pattern. Declared here rather than in the core layer
// because the core layer does not own an ID yet; this is the smoke test that the pattern compiles
// and that the implicit conversion it exists to forbid really is forbidden.
enum class ActorId : atlas::UInt64 {};

TEST(CoreTypes, FixedWidthAliasesHaveTheDeclaredWidth) {
    EXPECT_EQ(sizeof(atlas::Int8), std::size_t{1});
    EXPECT_EQ(sizeof(atlas::UInt8), std::size_t{1});
    EXPECT_EQ(sizeof(atlas::Int16), std::size_t{2});
    EXPECT_EQ(sizeof(atlas::UInt16), std::size_t{2});
    EXPECT_EQ(sizeof(atlas::Int32), std::size_t{4});
    EXPECT_EQ(sizeof(atlas::UInt32), std::size_t{4});
    EXPECT_EQ(sizeof(atlas::Int64), std::size_t{8});
    EXPECT_EQ(sizeof(atlas::UInt64), std::size_t{8});
    EXPECT_EQ(sizeof(atlas::Float32), std::size_t{4});
    EXPECT_EQ(sizeof(atlas::Float64), std::size_t{8});
    EXPECT_EQ(sizeof(atlas::Byte), std::size_t{1});
}

TEST(CoreTypes, SignednessMatchesTheAliasName) {
    EXPECT_TRUE(std::is_signed_v<atlas::Int32>);
    EXPECT_TRUE(std::is_signed_v<atlas::Int64>);
    EXPECT_FALSE(std::is_signed_v<atlas::UInt32>);
    EXPECT_FALSE(std::is_signed_v<atlas::UInt64>);
}

TEST(StrongTypedId, DoesNotConvertImplicitlyToItsUnderlyingType) {
    // The whole point of §4.3: swapping two ID arguments must be a compile error, not a silent
    // runtime bug. Explicit conversion still works and costs nothing at runtime.
    static_assert(!std::is_convertible_v<ActorId, atlas::UInt64>);
    static_assert(!std::is_convertible_v<atlas::UInt64, ActorId>);
    EXPECT_EQ(static_cast<atlas::UInt64>(ActorId{7}), atlas::UInt64{7});
}

TEST(CoreTime, SteadyAndWallClocksAreDistinctTypes) {
    // cpp-style.md §4.2 — mixing them is how a timeout turns into an infinite wait after an NTP
    // correction walks the wall clock backwards.
    static_assert(!std::is_same_v<atlas::Clock, atlas::SysClock>);
    static_assert(atlas::Clock::is_steady);
    EXPECT_GT(atlas::Millis{100}, atlas::Millis{1});
}

}  // namespace
