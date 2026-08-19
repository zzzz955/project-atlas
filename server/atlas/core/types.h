#pragma once

// =============================================================================
// [CS 4.1] 프로토콜 - 영속화 - ID 계층의 고정폭 정수 별칭
// int/long 은 LLP64 와 LP64 사이에서 폭이 갈리므로 금지
// =============================================================================

#include <cstddef>
#include <cstdint>

namespace atlas
{

using Int8 = std::int8_t;
using UInt8 = std::uint8_t;
using Int16 = std::int16_t;
using UInt16 = std::uint16_t;
using Int32 = std::int32_t;
using UInt32 = std::uint32_t;
using Int64 = std::int64_t;
using UInt64 = std::uint64_t;

using Float32 = float;
using Float64 = double;
using Byte = std::byte;

}  // namespace atlas
