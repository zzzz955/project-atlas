#pragma once
#include <cstddef>
#include <span>
#include <vector>

#include "atlas/core/types.h"

// architecture-design.md §8.1 — the frame layer.
//
// 🔴 This layer lives OUTSIDE atlas/net on purpose. `Session` defends a byte boundary (session.h,
// cpp-style.md §8): it reads and writes a byte stream and knows nothing about frames. Frames are
// the layer directly above it, so they get their own directory and the dependency edge points proto
// -> net, never the other way round.
//
// 🔴 The frame layer does NOT know what an opcode means. It carries a UInt16 and stops there. The
// opcode -> handler table is game-side (§14 core / contract / game); the core only ever accepts a
// registered callback. This is also the core-purity rule (§15.4): server/atlas/** may not include a
// generated game contract, which is why the little-endian helpers below are written out here rather
// than borrowed from generated/pkt/pkt_codec.h. They follow the same LE convention (§8.5).
//
// Wire layout, fixed 2026-08-10. 🔴 The table in §8.1 is the SoT for these widths:
//
//   0        2        4                8               12
//   +--------+--------+----------------+----------------+
//   | length | opcode |      seq       |     crc32      |  payload (length bytes) ...
//   +--------+--------+----------------+----------------+
//     UInt16   UInt16      UInt32           UInt32
//
//   length  UInt16 LE — PAYLOAD bytes only. The 12 header bytes are not counted.
//   opcode  UInt16 LE — message identifier, opaque here.
//   seq     UInt32 LE — per session, one counter each way (§8.3). Regression or a repeat closes
//                       the connection.
//   crc32   UInt32 LE — over opcode + seq + payload. 🔴 Framing integrity only, see crc32.h.
//
// 🔴 There is no 8-byte HMAC field and no placeholder for one. §8.2 layer 2 needs a session key,
// which drags in the §12 platform-auth handshake; a field reserved and filled with zeroes would
// only look like protection. No client exists yet, so widening the header from 12 to 20 bytes later
// costs exactly nothing today, and recording that fact is more honest than reserving the space
// (§16).
//
// 🔴 No packing pragma anywhere in this layer (cpp-style.md §4.4). Padding and alignment differ per
// compiler and platform; every field is written and read on its own.

namespace atlas {

// Fixed header size in bytes.
inline constexpr std::size_t kFrameHeaderSize = 12;

// architecture-design.md §9 — back-pressure, the half of the cap that belongs to a single message.
// The other half is Session::kMaxWriteQueueBytes. 🔴 Exceeding either closes the connection.
inline constexpr std::size_t kMaxPayload = std::size_t{16} * 1024;

// The decoded header. A plain aggregate — it is never memcpy'd on or off the wire.
struct FrameHeader {
    UInt16 length{0};
    UInt16 opcode{0};
    UInt32 seq{0};
    UInt32 crc32{0};
};

// One complete message: the header fields worth keeping, plus its payload.
struct Frame {
    UInt16 opcode{0};
    UInt32 seq{0};
    std::vector<Byte> payload;
};

// The checksummed region of §8.1: opcode, then seq, then the payload. The two header fields are
// folded in from their wire form, so the result matches whatever a peer computed over the bytes.
[[nodiscard]] UInt32 FrameChecksum(UInt16 opcode, UInt32 seq,
                                   std::span<const Byte> payload) noexcept;

// Appends one framed message to `out`. Returns false — and leaves `out` untouched — when the
// payload exceeds kMaxPayload, because a caller that overruns the cap has a bug the wire cannot
// express.
bool EncodeFrame(std::vector<Byte>& out, UInt16 opcode, UInt32 seq, std::span<const Byte> payload);

// Reads the fixed header out of `in`. Returns false when fewer than kFrameHeaderSize bytes are
// available. 🔴 The size check happens before any arithmetic on the size (cpp-style.md §4.4): an
// unsigned subtraction against a truncated buffer wraps to an enormous value.
bool DecodeFrameHeader(std::span<const Byte> in, FrameHeader& out) noexcept;

}  // namespace atlas
