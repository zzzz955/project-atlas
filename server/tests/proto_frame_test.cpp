#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <future>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/net/acceptor.h"
#include "atlas/net/io_runner.h"
#include "atlas/net/net_types.h"
#include "atlas/net/session.h"
#include "atlas/proto/crc32.h"
#include "atlas/proto/frame.h"
#include "atlas/proto/frame_reader.h"
#include "atlas/proto/session_framing.h"

namespace {

using atlas::Byte;
using atlas::Frame;
using atlas::FrameError;
using atlas::FrameReader;
using atlas::UInt16;
using atlas::UInt32;

std::vector<Byte> MakeBytes(std::string_view text) {
    std::vector<Byte> bytes;
    bytes.reserve(text.size());
    for (const char letter : text) {
        bytes.push_back(static_cast<Byte>(letter));
    }
    return bytes;
}

std::vector<Byte> MakeFrame(UInt16 opcode, UInt32 seq, std::span<const Byte> payload) {
    std::vector<Byte> out;
    EXPECT_TRUE(atlas::EncodeFrame(out, opcode, seq, payload));
    return out;
}

// Hand-rolls a header so a test can state a `length` the encoder would refuse to produce. This is
// the hostile peer: the prefix is whatever it says it is.
std::vector<Byte> MakeHeader(UInt16 length, UInt16 opcode, UInt32 seq, UInt32 crc32) {
    std::vector<Byte> out;
    const auto push16 = [&out](UInt16 value) {
        out.push_back(static_cast<Byte>(value & 0xFFU));
        out.push_back(static_cast<Byte>((value >> 8U) & 0xFFU));
    };
    const auto push32 = [&out](UInt32 value) {
        out.push_back(static_cast<Byte>(value & 0xFFU));
        out.push_back(static_cast<Byte>((value >> 8U) & 0xFFU));
        out.push_back(static_cast<Byte>((value >> 16U) & 0xFFU));
        out.push_back(static_cast<Byte>((value >> 24U) & 0xFFU));
    };
    push16(length);
    push16(opcode);
    push32(seq);
    push32(crc32);
    return out;
}

std::span<const Byte> Slice(const std::vector<Byte>& bytes, std::size_t offset, std::size_t count) {
    return std::span<const Byte>(bytes).subspan(offset, count);
}

// --------------------------------------------------------------------------------------------
// crc32 — architecture-design.md §8.1
// --------------------------------------------------------------------------------------------

// The canonical CRC-32/ISO-HDLC check value. A table generated with the wrong bit order still
// produces stable, self-consistent checksums, so only a published vector catches that mistake.
TEST(ProtoCrc32, MatchesTheCanonicalCheckValue) {
    const std::vector<Byte> data = MakeBytes("123456789");
    const UInt32 state = atlas::Crc32Update(atlas::kCrc32Init, data);
    EXPECT_EQ(atlas::Crc32Finish(state), UInt32{0xCBF43926U});
}

// The checksum covers opcode + seq + payload (§8.1), so a frame that differs only in its sequence
// number must not share a checksum — otherwise a replay would survive a corrupted seq field.
TEST(ProtoCrc32, ChecksumCoversTheHeaderFieldsNotJustThePayload) {
    const std::vector<Byte> payload = MakeBytes("same payload");
    EXPECT_NE(atlas::FrameChecksum(7, 1, payload), atlas::FrameChecksum(7, 2, payload));
    EXPECT_NE(atlas::FrameChecksum(7, 1, payload), atlas::FrameChecksum(8, 1, payload));
}

// --------------------------------------------------------------------------------------------
// EncodeFrame / DecodeFrameHeader
// --------------------------------------------------------------------------------------------

TEST(ProtoFrame, EncodesTheFixedTwelveByteHeader) {
    const std::vector<Byte> payload = MakeBytes("hello");
    const std::vector<Byte> wire = MakeFrame(0x1234, 0xDEADBEEF, payload);

    ASSERT_EQ(wire.size(), atlas::kFrameHeaderSize + payload.size());

    atlas::FrameHeader header{};
    ASSERT_TRUE(atlas::DecodeFrameHeader(wire, header));
    // 🔴 `length` counts the payload only — the header is not included.
    EXPECT_EQ(header.length, UInt16{5});
    EXPECT_EQ(header.opcode, UInt16{0x1234});
    EXPECT_EQ(header.seq, UInt32{0xDEADBEEF});
    EXPECT_EQ(header.crc32, atlas::FrameChecksum(0x1234, 0xDEADBEEF, payload));
}

TEST(ProtoFrame, DecodeHeaderRefusesATruncatedBuffer) {
    const std::vector<Byte> wire = MakeFrame(1, 1, MakeBytes("x"));
    atlas::FrameHeader header{};
    EXPECT_FALSE(atlas::DecodeFrameHeader(Slice(wire, 0, atlas::kFrameHeaderSize - 1), header));
}

TEST(ProtoFrame, EncodeRefusesAPayloadAboveTheCap) {
    const std::vector<Byte> payload(atlas::kMaxPayload + 1, Byte{0});
    std::vector<Byte> out;
    EXPECT_FALSE(atlas::EncodeFrame(out, 1, 1, payload));
    // 🔴 Nothing half-written: the next frame appended to `out` must not inherit a stray prefix.
    EXPECT_TRUE(out.empty());
}

// --------------------------------------------------------------------------------------------
// FrameReader — the reassembly state machine
// --------------------------------------------------------------------------------------------

TEST(ProtoFrameReader, ParsesOneCompleteFrame) {
    const std::vector<Byte> payload = MakeBytes("atlas frame");
    const std::vector<Byte> wire = MakeFrame(42, 1, payload);

    FrameReader reader;
    std::vector<Frame> frames;
    ASSERT_EQ(reader.Feed(wire, frames), FrameError::None);

    ASSERT_EQ(frames.size(), std::size_t{1});
    EXPECT_EQ(frames[0].opcode, UInt16{42});
    EXPECT_EQ(frames[0].seq, UInt32{1});
    EXPECT_EQ(frames[0].payload, payload);
    // The cursor is spent: nothing is held back.
    EXPECT_EQ(reader.Pending(), std::size_t{0});
}

TEST(ProtoFrameReader, KeepsStateWhenTheHeaderIsSplit) {
    const std::vector<Byte> payload = MakeBytes("split header");
    const std::vector<Byte> wire = MakeFrame(9, 1, payload);

    FrameReader reader;
    std::vector<Frame> frames;

    // Five bytes: mid-header, before `seq` has even started.
    ASSERT_EQ(reader.Feed(Slice(wire, 0, 5), frames), FrameError::None);
    EXPECT_TRUE(frames.empty());
    EXPECT_EQ(reader.Pending(), std::size_t{5});

    ASSERT_EQ(reader.Feed(Slice(wire, 5, wire.size() - 5), frames), FrameError::None);
    ASSERT_EQ(frames.size(), std::size_t{1});
    EXPECT_EQ(frames[0].payload, payload);
    EXPECT_EQ(reader.Pending(), std::size_t{0});
}

TEST(ProtoFrameReader, KeepsStateWhenThePayloadIsSplit) {
    const std::vector<Byte> payload = MakeBytes("split payload across two reads");
    const std::vector<Byte> wire = MakeFrame(9, 1, payload);
    const std::size_t cut = atlas::kFrameHeaderSize + 4;

    FrameReader reader;
    std::vector<Frame> frames;

    ASSERT_EQ(reader.Feed(Slice(wire, 0, cut), frames), FrameError::None);
    EXPECT_TRUE(frames.empty());
    EXPECT_EQ(reader.Pending(), cut);

    ASSERT_EQ(reader.Feed(Slice(wire, cut, wire.size() - cut), frames), FrameError::None);
    ASSERT_EQ(frames.size(), std::size_t{1});
    EXPECT_EQ(frames[0].payload, payload);
}

// One byte at a time is the pathological split: every boundary in the header is exercised.
TEST(ProtoFrameReader, ReassemblesAByteAtATime) {
    const std::vector<Byte> payload = MakeBytes("drip");
    const std::vector<Byte> wire = MakeFrame(3, 1, payload);

    FrameReader reader;
    std::vector<Frame> frames;
    for (std::size_t i = 0; i < wire.size(); ++i) {
        ASSERT_EQ(reader.Feed(Slice(wire, i, 1), frames), FrameError::None);
        EXPECT_EQ(frames.size(), i + 1 == wire.size() ? std::size_t{1} : std::size_t{0});
    }
    ASSERT_EQ(frames.size(), std::size_t{1});
    EXPECT_EQ(frames[0].payload, payload);
}

TEST(ProtoFrameReader, ParsesThreeFramesFromOneBufferInOrder) {
    const std::array<std::string_view, 3> texts{"one", "two", "three"};
    std::vector<Byte> wire;
    for (UInt32 index = 1; index <= 3; ++index) {
        const std::vector<Byte> payload = MakeBytes(texts[index - 1]);
        const std::vector<Byte> one = MakeFrame(static_cast<UInt16>(100 + index), index, payload);
        wire.insert(wire.end(), one.begin(), one.end());
    }

    FrameReader reader;
    std::vector<Frame> frames;
    ASSERT_EQ(reader.Feed(wire, frames), FrameError::None);

    ASSERT_EQ(frames.size(), std::size_t{3});
    EXPECT_EQ(frames[0].opcode, UInt16{101});
    EXPECT_EQ(frames[1].opcode, UInt16{102});
    EXPECT_EQ(frames[2].opcode, UInt16{103});
    EXPECT_EQ(frames[2].payload, MakeBytes("three"));
    EXPECT_EQ(reader.Pending(), std::size_t{0});
}

// 🔴 The cap is checked before the remaining-size comparison and before any subspan, so the reader
// never reserves room for a length it has not seen delivered.
TEST(ProtoFrameReader, RejectsALengthAboveTheCap) {
    const std::vector<Byte> wire = MakeHeader(static_cast<UInt16>(atlas::kMaxPayload + 1), 1, 1, 0);

    FrameReader reader;
    std::vector<Frame> frames;
    EXPECT_EQ(reader.Feed(wire, frames), FrameError::PayloadTooLarge);
    EXPECT_TRUE(frames.empty());
}

// A prefix that overstates what has arrived, but stays inside the cap, is not an error — it is the
// ordinary case of a payload still in flight. Treating it as an error would close healthy sessions.
TEST(ProtoFrameReader, ALyingPrefixWithinTheCapOnlyWaits) {
    std::vector<Byte> wire = MakeHeader(1000, 1, 1, 0);
    const std::vector<Byte> partial = MakeBytes("only ten b");
    wire.insert(wire.end(), partial.begin(), partial.end());

    FrameReader reader;
    std::vector<Frame> frames;
    EXPECT_EQ(reader.Feed(wire, frames), FrameError::None);
    EXPECT_TRUE(frames.empty());
    EXPECT_EQ(reader.Pending(), wire.size());
}

TEST(ProtoFrameReader, RejectsASingleFlippedBit) {
    const std::vector<Byte> payload = MakeBytes("integrity");
    std::vector<Byte> wire = MakeFrame(5, 1, payload);

    // One bit, in the payload, leaving every header field intact.
    wire[atlas::kFrameHeaderSize] = wire[atlas::kFrameHeaderSize] ^ Byte { 0x01 };

    FrameReader reader;
    std::vector<Frame> frames;
    EXPECT_EQ(reader.Feed(wire, frames), FrameError::ChecksumMismatch);
    EXPECT_TRUE(frames.empty());
}

TEST(ProtoFrameReader, RejectsARepeatedSequenceNumber) {
    const std::vector<Byte> payload = MakeBytes("replay");
    const std::vector<Byte> first = MakeFrame(1, 7, payload);

    FrameReader reader;
    std::vector<Frame> frames;
    ASSERT_EQ(reader.Feed(first, frames), FrameError::None);
    ASSERT_EQ(frames.size(), std::size_t{1});

    EXPECT_EQ(reader.Feed(first, frames), FrameError::SequenceRegression);
    // The duplicate itself is not delivered.
    EXPECT_EQ(frames.size(), std::size_t{1});
}

TEST(ProtoFrameReader, RejectsASequenceNumberThatGoesBackwards) {
    const std::vector<Byte> payload = MakeBytes("out of order");

    FrameReader reader;
    std::vector<Frame> frames;
    ASSERT_EQ(reader.Feed(MakeFrame(1, 10, payload), frames), FrameError::None);
    EXPECT_EQ(reader.Feed(MakeFrame(1, 9, payload), frames), FrameError::SequenceRegression);
    EXPECT_EQ(frames.size(), std::size_t{1});
}

// Frames that decoded before the bad one are still handed over: they were valid when they arrived,
// and dropping them would lose work the peer already paid for.
TEST(ProtoFrameReader, DeliversTheGoodFramesAheadOfAFailure) {
    const std::vector<Byte> good = MakeFrame(1, 1, MakeBytes("good"));
    std::vector<Byte> bad = MakeFrame(1, 2, MakeBytes("bad"));
    bad[atlas::kFrameHeaderSize] = bad[atlas::kFrameHeaderSize] ^ Byte { 0x80 };

    std::vector<Byte> wire = good;
    wire.insert(wire.end(), bad.begin(), bad.end());

    FrameReader reader;
    std::vector<Frame> frames;
    EXPECT_EQ(reader.Feed(wire, frames), FrameError::ChecksumMismatch);
    ASSERT_EQ(frames.size(), std::size_t{1});
    EXPECT_EQ(frames[0].payload, MakeBytes("good"));
}

TEST(ProtoFrameReader, AcceptsAMaximumSizedPayload) {
    const std::vector<Byte> payload(atlas::kMaxPayload, Byte{0xAB});
    const std::vector<Byte> wire = MakeFrame(1, 1, payload);

    FrameReader reader;
    std::vector<Frame> frames;
    ASSERT_EQ(reader.Feed(wire, frames), FrameError::None);
    ASSERT_EQ(frames.size(), std::size_t{1});
    EXPECT_EQ(frames[0].payload.size(), atlas::kMaxPayload);
}

// --------------------------------------------------------------------------------------------
// The seam: bytes -> frames over the real socket stack
// --------------------------------------------------------------------------------------------

template <class Predicate>
bool WaitUntil(Predicate predicate) {
    const atlas::TimePoint deadline = atlas::Clock::now() + atlas::Seconds{5};
    while (!predicate()) {
        if (atlas::Clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

// A synchronous client driven by the test thread, so the assertions read top to bottom.
class TestClient {
public:
    explicit TestClient(const atlas::Endpoint& endpoint) : socket_(io_context_) {
        socket_.connect(endpoint);
    }

    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;

    ~TestClient() {
        atlas::ErrorCode ignored;
        std::ignore = socket_.shutdown(atlas::Socket::shutdown_both, ignored);
        std::ignore = socket_.close(ignored);
    }

    void Write(const std::vector<Byte>& bytes) {
        atlas::asio::write(socket_, atlas::asio::buffer(bytes));
    }

    std::vector<Byte> Read(std::size_t count) {
        std::vector<Byte> bytes(count);
        atlas::asio::read(socket_, atlas::asio::buffer(bytes));
        return bytes;
    }

private:
    atlas::IoContext io_context_;
    atlas::Socket socket_;
};

// An echo server that speaks frames rather than bytes: AttachFrameReader is the only thing standing
// between Session's byte boundary and the handler below.
class ProtoSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        accepted_future_ = accepted_.get_future();
        runner_ = std::make_unique<atlas::IoRunner>(2);
        // Loopback rather than the any-address, and port 0 so parallel ctest runs cannot collide.
        acceptor_ = std::make_unique<atlas::SessionAcceptor>(
            runner_->Context(), atlas::Endpoint(atlas::asio::ip::address_v4::loopback(), 0),
            [this](const std::shared_ptr<atlas::Session>& session) { OnAccept(session); });
        acceptor_->Start();
        runner_->Start();
    }

    void TearDown() override {
        // architecture-design.md §9 — front door first, then the pool.
        acceptor_->Stop();
        runner_->Stop();
    }

    void OnAccept(const std::shared_ptr<atlas::Session>& session) {
        // 🔴 The acceptor calls this after Session::Create and before Session::Start(), which is
        // exactly the window in which a bytes handler may still be installed.
        atlas::AttachFrameReader(
            session, [this](const std::shared_ptr<atlas::Session>& source, const Frame& frame) {
                received_.fetch_add(1);
                // Echoed back under this side's own send counter (§8.3 keeps one per direction).
                // Runs on the session strand, so the counter needs no synchronisation.
                ++send_seq_;
                atlas::SendFrame(*source, frame.opcode, send_seq_, frame.payload);
            });
        session->SetCloseHandler(
            [this](const std::shared_ptr<atlas::Session>&) { closed_count_.fetch_add(1); });

        // Runs on the accept strand; one session per test, so a plain flag is correct.
        if (!accepted_set_) {
            accepted_set_ = true;
            accepted_.set_value(session);
        }
    }

    std::shared_ptr<atlas::Session> TakeSession() {
        if (accepted_future_.wait_for(atlas::Seconds{5}) != std::future_status::ready) {
            return nullptr;
        }
        return accepted_future_.get();
    }

    // 🔴 Declaration order is the destruction contract: runner_ goes last.
    std::unique_ptr<atlas::IoRunner> runner_;
    std::unique_ptr<atlas::SessionAcceptor> acceptor_;
    std::promise<std::shared_ptr<atlas::Session>> accepted_;
    std::future<std::shared_ptr<atlas::Session>> accepted_future_;
    bool accepted_set_{false};
    UInt32 send_seq_{0};
    std::atomic<UInt32> received_{0};
    std::atomic<UInt32> closed_count_{0};
};

TEST_F(ProtoSessionTest, FramedRoundTripOverARealSocket) {
    TestClient client(acceptor_->LocalEndpoint());

    const std::vector<Byte> payload = MakeBytes("bytes in, frames out");
    client.Write(MakeFrame(77, 1, payload));

    const std::vector<Byte> response = client.Read(atlas::kFrameHeaderSize + payload.size());

    FrameReader reader;
    std::vector<Frame> frames;
    ASSERT_EQ(reader.Feed(response, frames), FrameError::None);
    ASSERT_EQ(frames.size(), std::size_t{1});
    EXPECT_EQ(frames[0].opcode, UInt16{77});
    EXPECT_EQ(frames[0].payload, payload);
    EXPECT_EQ(received_.load(), UInt32{1});
}

// Two frames written as one TCP write: the reader has to split what the socket merged.
TEST_F(ProtoSessionTest, MergedWritesAreSplitBackIntoFrames) {
    TestClient client(acceptor_->LocalEndpoint());

    std::vector<Byte> wire = MakeFrame(1, 1, MakeBytes("first"));
    const std::vector<Byte> second = MakeFrame(2, 2, MakeBytes("second"));
    wire.insert(wire.end(), second.begin(), second.end());
    client.Write(wire);

    ASSERT_TRUE(WaitUntil([this] { return received_.load() >= 2; }));
}

// 🔴 architecture-design.md §8.1 / §8.3 — a framing failure closes the connection. This is the wire
// that proves AttachFrameReader acts on the reader's verdict instead of logging and carrying on.
TEST_F(ProtoSessionTest, AChecksumFailureClosesTheConnection) {
    TestClient client(acceptor_->LocalEndpoint());

    std::vector<Byte> wire = MakeFrame(1, 1, MakeBytes("corrupt me"));
    wire[atlas::kFrameHeaderSize] = wire[atlas::kFrameHeaderSize] ^ Byte { 0x01 };
    client.Write(wire);

    EXPECT_TRUE(WaitUntil([this] { return closed_count_.load() >= 1; }));
    EXPECT_EQ(received_.load(), UInt32{0});
}

// 🔴 architecture-design.md §9 — the back-pressure cap. A single payload above the queue ceiling is
// the deterministic form of the check: it can never fit, so no cooperation from the peer's receive
// window is needed to observe the decision. And the decision is a close, not a drop.
TEST_F(ProtoSessionTest, WriteQueueOverflowClosesTheConnection) {
    TestClient client(acceptor_->LocalEndpoint());
    const std::shared_ptr<atlas::Session> session = TakeSession();
    ASSERT_NE(session, nullptr);

    const std::vector<Byte> oversized(atlas::Session::kMaxWriteQueueBytes + 1, Byte{0});
    session->Send(oversized);

    EXPECT_TRUE(WaitUntil([this] { return closed_count_.load() >= 1; }));
}

}  // namespace
