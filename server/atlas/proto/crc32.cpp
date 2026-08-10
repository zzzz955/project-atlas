#include "atlas/proto/crc32.h"

#include <array>
#include <cstddef>

namespace atlas {
namespace {

// 🔴 The names carry the `Crc32` prefix even inside an anonymous namespace. Unity builds fuse up to
// eight sources into one translation unit (cpp-style.md §2.2), and file-scope isolation is exactly
// what that fusion removes — a generic `MakeTable` here would collide with the next file's.
constexpr std::size_t kCrc32TableSize = 256;

constexpr std::array<UInt32, kCrc32TableSize> MakeCrc32Table() {
    std::array<UInt32, kCrc32TableSize> table{};
    for (UInt32 index = 0; index < static_cast<UInt32>(kCrc32TableSize); ++index) {
        UInt32 remainder = index;
        for (UInt32 bit = 0; bit < 8U; ++bit) {
            remainder =
                ((remainder & 1U) != 0U) ? (0xEDB88320U ^ (remainder >> 1U)) : (remainder >> 1U);
        }
        table[index] = remainder;
    }
    return table;
}

// Built at compile time, so there is no initialisation order question and no runtime setup cost.
constexpr std::array<UInt32, kCrc32TableSize> kCrc32Table = MakeCrc32Table();

}  // namespace

UInt32 Crc32Update(UInt32 state, std::span<const Byte> data) noexcept {
    UInt32 remainder = state;
    for (const Byte value : data) {
        const UInt32 index = (remainder ^ std::to_integer<UInt32>(value)) & 0xFFU;
        remainder = kCrc32Table[static_cast<std::size_t>(index)] ^ (remainder >> 8U);
    }
    return remainder;
}

UInt32 Crc32Finish(UInt32 state) noexcept { return state ^ 0xFFFFFFFFU; }

}  // namespace atlas
