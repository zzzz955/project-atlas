#include "atlas/proto/frame.h"

#include <array>

#include "atlas/proto/crc32.h"

namespace atlas {
namespace {

// 🔴 `Frame` prefix, anonymous namespace notwithstanding: a unity build fuses this file with its
// neighbours into one translation unit, so a bare `PutLe16` would collide (cpp-style.md §2.2).
// These duplicate what generated/pkt/pkt_codec.h does for payload fields, and that duplication is
// deliberate — core-purity (§15.4) forbids server/atlas/** from including a generated header. Both
// sides follow the same fixed little-endian convention of §8.5.

void FramePutLe16(std::vector<Byte>& out, UInt16 value) {
    out.push_back(static_cast<Byte>(value & 0xFFU));
    out.push_back(static_cast<Byte>((value >> 8U) & 0xFFU));
}

void FramePutLe32(std::vector<Byte>& out, UInt32 value) {
    out.push_back(static_cast<Byte>(value & 0xFFU));
    out.push_back(static_cast<Byte>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<Byte>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<Byte>((value >> 24U) & 0xFFU));
}

// Both readers are called only after the caller has proved the bytes are there.
UInt16 FrameGetLe16(std::span<const Byte> in) noexcept {
    return static_cast<UInt16>(std::to_integer<UInt16>(in[0]) |
                               static_cast<UInt16>(std::to_integer<UInt16>(in[1]) << 8U));
}

UInt32 FrameGetLe32(std::span<const Byte> in) noexcept {
    return std::to_integer<UInt32>(in[0]) | (std::to_integer<UInt32>(in[1]) << 8U) |
           (std::to_integer<UInt32>(in[2]) << 16U) | (std::to_integer<UInt32>(in[3]) << 24U);
}

}  // namespace

UInt32 FrameChecksum(UInt16 opcode, UInt32 seq, std::span<const Byte> payload) noexcept {
    // The checksummed region of §8.1 is a virtual concatenation: header bytes 2..8 (opcode, seq),
    // then the payload. Six bytes on the stack reproduce that prefix without copying the payload.
    const std::array<Byte, 6> prefix{
        static_cast<Byte>(opcode & 0xFFU),       static_cast<Byte>((opcode >> 8U) & 0xFFU),
        static_cast<Byte>(seq & 0xFFU),          static_cast<Byte>((seq >> 8U) & 0xFFU),
        static_cast<Byte>((seq >> 16U) & 0xFFU), static_cast<Byte>((seq >> 24U) & 0xFFU),
    };

    UInt32 state = Crc32Update(kCrc32Init, std::span<const Byte>(prefix));
    state = Crc32Update(state, payload);
    return Crc32Finish(state);
}

bool EncodeFrame(std::vector<Byte>& out, UInt16 opcode, UInt32 seq, std::span<const Byte> payload) {
    // 🔴 Checked before anything is appended, so a rejected message leaves no half-written frame
    // behind for the next Send to concatenate itself onto.
    if (payload.size() > kMaxPayload) {
        return false;
    }

    // kMaxPayload (16 KiB) is far below the UInt16 ceiling, so the narrowing cast cannot lose data.
    const UInt16 length = static_cast<UInt16>(payload.size());

    out.reserve(out.size() + kFrameHeaderSize + payload.size());
    FramePutLe16(out, length);
    FramePutLe16(out, opcode);
    FramePutLe32(out, seq);
    FramePutLe32(out, FrameChecksum(opcode, seq, payload));
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

bool DecodeFrameHeader(std::span<const Byte> in, FrameHeader& out) noexcept {
    if (in.size() < kFrameHeaderSize) {
        return false;
    }

    out.length = FrameGetLe16(in.subspan(0, 2));
    out.opcode = FrameGetLe16(in.subspan(2, 2));
    out.seq = FrameGetLe32(in.subspan(4, 4));
    out.crc32 = FrameGetLe32(in.subspan(8, 4));
    return true;
}

}  // namespace atlas
