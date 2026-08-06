// 🔴 HAND-WRITTEN. This is the only .cpp under server/generated/ that is not generator output —
//    it is the gate ON the generator. It lives here rather than in server/tests/ so that the
//    generator work item owns one directory end to end and never collides with the core tests.
//
// What it proves (architecture-design.md §8.4 / §8.5, cpp-style.md §4.4):
//   1. Round trip      — a fully populated DTO survives Write -> Read unchanged.
//   2. Truncation      — every strict prefix of a valid buffer makes Read return false, without
//                        throwing or crashing, and without touching the destination DTO.
//   3. Byte order      — known values land as the expected LITTLE-ENDIAN byte sequence, and the
//                        variable-length prefix is UInt16 counting BYTES for UTF-8 text.
//   4. FP rejection    — see tests/CMakeLists.txt: driven through CTest against the generator.

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atlas/core/types.h"
#include "generated/pkt/chat_broadcast.h"
#include "generated/pkt/contract_enums.h"
#include "generated/pkt/move_input_request.h"
#include "generated/pkt/move_state_broadcast.h"
#include "generated/pkt/pkt_codec.h"

namespace {

using atlas::Byte;
using atlas::Int16;
using atlas::UInt8;
using atlas::UInt16;
using atlas::UInt32;
using atlas::UInt64;
using atlas::generated::ChatBroadcast;
using atlas::generated::ChatChannel;
using atlas::generated::MoveDirection;
using atlas::generated::MoveInputRequest;
using atlas::generated::MoveStateBroadcast;

std::vector<Byte> ToBytes(const std::vector<UInt8>& values) {
    std::vector<Byte> bytes;
    bytes.reserve(values.size());
    for (const UInt8 value : values) bytes.push_back(static_cast<Byte>(value));
    return bytes;
}

std::vector<UInt8> FromBytes(const std::vector<Byte>& bytes) {
    std::vector<UInt8> values;
    values.reserve(bytes.size());
    for (const Byte byte : bytes) values.push_back(std::to_integer<UInt8>(byte));
    return values;
}

// Write -> Read -> compare, and assert the reader consumed exactly what the writer produced.
template <class Dto>
void ExpectRoundTrip(const Dto& source) {
    std::vector<Byte> bytes;
    source.Write(bytes);
    ASSERT_FALSE(bytes.empty());

    std::span<const Byte> cursor{bytes};
    Dto restored;
    ASSERT_TRUE(restored.Read(cursor));
    EXPECT_TRUE(restored == source);
    EXPECT_TRUE(cursor.empty()) << "Read left " << cursor.size() << " byte(s) unconsumed";
}

// Every strict prefix of a valid encoding must be rejected. This is the underflow gate: the
// codec checks the remaining size before every subtraction, so a short buffer can never wrap
// into a huge length (cpp-style.md §4.4).
template <class Dto>
void ExpectTruncationRejected(const Dto& source) {
    std::vector<Byte> bytes;
    source.Write(bytes);

    for (std::size_t length = 0; length < bytes.size(); ++length) {
        std::span<const Byte> cursor{bytes.data(), length};
        const Dto untouched;
        Dto target;
        // 🔴 No EXPECT_THROW anywhere here: a parse failure is a return value, never an
        //    exception (architecture-design.md §11.2a).
        EXPECT_FALSE(target.Read(cursor)) << "accepted a buffer truncated to " << length << " byte(s)";
        EXPECT_EQ(cursor.size(), length) << "failed Read advanced the span at length " << length;
        EXPECT_TRUE(target == untouched) << "failed Read mutated the destination at length " << length;
    }
}

MoveInputRequest SampleMoveInput() {
    MoveInputRequest dto;
    dto.client_tick_ = UInt32{0x01020304};
    dto.direction_ = MoveDirection::East;
    dto.grid_x_ = Int16{-2};
    dto.grid_y_ = Int16{258};
    dto.sprinting_ = true;
    return dto;
}

MoveStateBroadcast SampleMoveState() {
    MoveStateBroadcast dto;
    dto.server_tick_ = UInt32{0xDEADBEEF};
    dto.actor_ids_ = {UInt64{1}, UInt64{0xFFFFFFFFFFFFFFFFULL}, UInt64{42}};
    dto.grid_xs_ = {Int16{0}, Int16{-32768}, Int16{32767}};
    dto.grid_ys_ = {Int16{7}, Int16{-1}, Int16{0}};
    return dto;
}

ChatBroadcast SampleChat() {
    ChatBroadcast dto;
    dto.speaker_actor_id_ = UInt64{0x0102030405060708ULL};
    dto.channel_ = ChatChannel::World;
    dto.text_ = "hi";
    return dto;
}

// ── 1. Round trip ────────────────────────────────────────────────────────────────
TEST(PktRoundTrip, MoveInputRequest) { ExpectRoundTrip(SampleMoveInput()); }

TEST(PktRoundTrip, MoveStateBroadcastWithRepeatedFields) { ExpectRoundTrip(SampleMoveState()); }

TEST(PktRoundTrip, ChatBroadcastWithText) { ExpectRoundTrip(SampleChat()); }

TEST(PktRoundTrip, DefaultConstructedIsStable) {
    ExpectRoundTrip(MoveInputRequest{});
    ExpectRoundTrip(MoveStateBroadcast{});
    ExpectRoundTrip(ChatBroadcast{});
}

TEST(PktRoundTrip, ReadStopsAtTheEndOfItsOwnPayload) {
    // Two DTOs back to back: the first Read must consume exactly its own bytes and hand the
    // rest over untouched. This is what makes the DTOs usable inside a future frame.
    std::vector<Byte> bytes;
    const MoveInputRequest first = SampleMoveInput();
    const ChatBroadcast second = SampleChat();
    first.Write(bytes);
    second.Write(bytes);

    std::span<const Byte> cursor{bytes};
    MoveInputRequest restored_first;
    ChatBroadcast restored_second;
    ASSERT_TRUE(restored_first.Read(cursor));
    ASSERT_TRUE(restored_second.Read(cursor));
    EXPECT_TRUE(restored_first == first);
    EXPECT_TRUE(restored_second == second);
    EXPECT_TRUE(cursor.empty());
}

// ── 2. Truncated input returns false ─────────────────────────────────────────────
TEST(PktTruncation, MoveInputRequest) { ExpectTruncationRejected(SampleMoveInput()); }

TEST(PktTruncation, MoveStateBroadcastWithRepeatedFields) {
    ExpectTruncationRejected(SampleMoveState());
}

TEST(PktTruncation, ChatBroadcastWithText) { ExpectTruncationRejected(SampleChat()); }

TEST(PktTruncation, LyingLengthPrefixIsRejectedNotAllocated) {
    // A hostile prefix claiming 65535 elements with nothing behind it must fail on the buffer
    // check, not on an allocation. If this ever hangs or dies, the codec started reserving.
    std::vector<Byte> bytes = ToBytes({0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF});
    std::span<const Byte> cursor{bytes};
    MoveStateBroadcast target;
    EXPECT_FALSE(target.Read(cursor));

    // Same story for text: 8 zero id bytes + zero channel + a prefix claiming 4096 bytes, and
    // then nothing at all behind it.
    std::vector<Byte> text_bytes =
        ToBytes({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10});
    std::span<const Byte> text_cursor{text_bytes};
    ChatBroadcast chat_target;
    EXPECT_FALSE(chat_target.Read(text_cursor));
}

// ── 3. Little-endian byte order ──────────────────────────────────────────────────
TEST(PktByteOrder, MoveInputRequestIsLittleEndian) {
    std::vector<Byte> bytes;
    SampleMoveInput().Write(bytes);

    const std::vector<UInt8> expected = {
        0x04, 0x03, 0x02, 0x01,  // UInt32 client_tick_ = 0x01020304, least significant byte first
        0x02,                    // MoveDirection::East, one byte (underlying UInt8)
        0xFE, 0xFF,              // Int16 grid_x_ = -2  -> 0xFFFE little-endian
        0x02, 0x01,              // Int16 grid_y_ = 258 -> 0x0102 little-endian
        0x01,                    // bool sprinting_ = true, exactly one byte
    };
    EXPECT_EQ(FromBytes(bytes), expected);
}

TEST(PktByteOrder, ChatBroadcastPrefixesTextWithUInt16ByteCount) {
    std::vector<Byte> bytes;
    SampleChat().Write(bytes);

    const std::vector<UInt8> expected = {
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,  // UInt64 speaker id, little-endian
        0x01,                                            // ChatChannel::World
        0x02, 0x00,                                      // UInt16 length prefix = 2 bytes
        0x68, 0x69,                                      // "hi" as UTF-8
    };
    EXPECT_EQ(FromBytes(bytes), expected);
}

TEST(PktByteOrder, TextPrefixCountsBytesNotCodePoints) {
    // U+AC00 is three bytes in UTF-8. The prefix must say 3, not 1 — the encoding is UTF-8 and
    // the prefix counts bytes (architecture-design.md §8.5). Spelled as escapes so the result
    // does not depend on the compiler's source charset.
    ChatBroadcast dto;
    dto.text_ = std::string("\xEA\xB0\x80");
    ASSERT_EQ(dto.text_.size(), std::size_t{3});

    std::vector<Byte> bytes;
    dto.Write(bytes);
    const std::vector<UInt8> expected = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // speaker id 0
        0x00,                                            // ChatChannel::Say
        0x03, 0x00,                                      // UInt16 prefix = 3 BYTES, not 1 glyph
        0xEA, 0xB0, 0x80,                                // U+AC00 in UTF-8
    };
    EXPECT_EQ(FromBytes(bytes), expected);

    ExpectRoundTrip(dto);
}

TEST(PktByteOrder, RepeatedFieldIsCountPrefixedThenElements) {
    MoveStateBroadcast dto;
    dto.server_tick_ = UInt32{0};
    dto.actor_ids_ = {UInt64{0x0807060504030201ULL}};

    std::vector<Byte> bytes;
    dto.Write(bytes);
    const std::vector<UInt8> expected = {
        0x00, 0x00, 0x00, 0x00,                          // server_tick_
        0x01, 0x00,                                      // UInt16 element count = 1
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // the UInt64, little-endian
        0x00, 0x00,                                      // grid_xs_ count = 0
        0x00, 0x00,                                      // grid_ys_ count = 0
    };
    EXPECT_EQ(FromBytes(bytes), expected);
}

TEST(PktByteOrder, EnumWidthMatchesTheContract) {
    // MoveDirection is declared `: byte` in the contract, so it must occupy exactly one byte.
    // If the generator ever dropped the underlying type, this size would move.
    std::vector<Byte> narrow;
    std::vector<Byte> wide;
    MoveInputRequest dto;
    dto.direction_ = MoveDirection::None;
    dto.Write(narrow);
    dto.direction_ = MoveDirection::West;
    dto.Write(wide);
    ASSERT_EQ(narrow.size(), wide.size());
    EXPECT_EQ(FromBytes(wide)[4], UInt8{0x04});
}

}  // namespace
