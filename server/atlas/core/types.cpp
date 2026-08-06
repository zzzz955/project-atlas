#include "atlas/core/types.h"

#include <type_traits>

namespace atlas {

// cpp-style.md §4.1 — Windows is LLP64, Linux is LP64. These assertions are the regression guard:
// if an alias ever changes width between dev and prod, the build stops here instead of the wire
// format and the DB rows silently disagreeing.
static_assert(sizeof(Int8) == 1 && sizeof(UInt8) == 1);
static_assert(sizeof(Int16) == 2 && sizeof(UInt16) == 2);
static_assert(sizeof(Int32) == 4 && sizeof(UInt32) == 4);
static_assert(sizeof(Int64) == 8 && sizeof(UInt64) == 8);
static_assert(sizeof(Float32) == 4);
static_assert(sizeof(Float64) == 8);
static_assert(sizeof(Byte) == 1);

static_assert(std::is_signed_v<Int8> && std::is_signed_v<Int16>);
static_assert(std::is_signed_v<Int32> && std::is_signed_v<Int64>);
static_assert(std::is_unsigned_v<UInt8> && std::is_unsigned_v<UInt16>);
static_assert(std::is_unsigned_v<UInt32> && std::is_unsigned_v<UInt64>);

}  // namespace atlas
