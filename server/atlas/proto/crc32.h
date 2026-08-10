#pragma once
#include <span>

#include "atlas/core/types.h"

// architecture-design.md §8.1 / §8.2 — CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
//
// 🔴 THIS IS NOT TAMPER PROTECTION. §8.2 layer 1 and nothing above it. The algorithm is public, so
// an adversary who rewrites a payload simply recomputes the checksum and the frame verifies; TCP
// has already checksummed the same bytes on the way in. What this buys is *framing* integrity —
// proof that the length prefix this reader trusted actually delimits the frame that was sent, so a
// desynchronised stream is caught at the first frame instead of being reinterpreted forever after.
// Tampering is stopped by the session-key HMAC (§8.2 layer 2, not built) and the last line is
// server authority (§8.2 layer 3). 🔴 "The checksum detects tampering" is false; do not write it.
//
// The streaming form exists because the checksummed region is a virtual concatenation — the opcode
// and sequence fields out of the header, then the payload — and materialising that into one buffer
// just to hash it would copy every payload an extra time.

namespace atlas {

// Initial register value. Fold the data in with Crc32Update, then Crc32Finish once at the end.
inline constexpr UInt32 kCrc32Init = 0xFFFFFFFFU;

[[nodiscard]] UInt32 Crc32Update(UInt32 state, std::span<const Byte> data) noexcept;

[[nodiscard]] UInt32 Crc32Finish(UInt32 state) noexcept;

}  // namespace atlas
