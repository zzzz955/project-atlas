#include "atlas/proto/frame_reader.h"

#include <utility>

namespace atlas {

FrameError FrameReader::Feed(std::span<const Byte> bytes, std::vector<Frame>& out) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    for (;;) {
        const std::size_t available = buffer_.size() - cursor_;
        if (available < kFrameHeaderSize) {
            break;  // A header split across reads: keep the bytes and wait for the rest.
        }

        const std::span<const Byte> rest =
            std::span<const Byte>(buffer_).subspan(cursor_, available);

        FrameHeader header{};
        if (!DecodeFrameHeader(rest, header)) {
            break;  // Unreachable given the check above; kept so the contract is not assumed away.
        }

        const std::size_t declared = static_cast<std::size_t>(header.length);

        // 🔴 Validation order matters (cpp-style.md §4.4, the same rule pkt_codec.h follows):
        //    cap first, remaining-size second, and only then a subspan. The prefix is chosen by the
        //    peer, so a lie must die on a comparison and never reach an allocation.
        if (declared > kMaxPayload) {
            return FrameError::PayloadTooLarge;
        }
        // `available >= kFrameHeaderSize` was established above, so this subtraction cannot wrap.
        if (available - kFrameHeaderSize < declared) {
            break;  // A truthful prefix whose payload has not all arrived. Not an error.
        }

        const std::span<const Byte> payload = rest.subspan(kFrameHeaderSize, declared);

        if (FrameChecksum(header.opcode, header.seq, payload) != header.crc32) {
            return FrameError::ChecksumMismatch;
        }

        // architecture-design.md §8.3 — one receive counter per session; a repeat or a step
        // backwards is a replay or a desynchronised stream either way, and both close the
        // connection. The first frame of a connection establishes the baseline.
        if (has_last_seq_ && header.seq <= last_seq_) {
            return FrameError::SequenceRegression;
        }
        last_seq_ = header.seq;
        has_last_seq_ = true;

        Frame frame;
        frame.opcode = header.opcode;
        frame.seq = header.seq;
        frame.payload.assign(payload.begin(), payload.end());
        out.push_back(std::move(frame));

        cursor_ += kFrameHeaderSize + declared;
    }

    DropConsumed();
    return FrameError::None;
}

std::size_t FrameReader::Pending() const noexcept { return buffer_.size() - cursor_; }

std::string_view FrameReader::Describe(FrameError error) noexcept {
    switch (error) {
        case FrameError::None:
            return "none";
        case FrameError::PayloadTooLarge:
            return "payload above kMaxPayload";
        case FrameError::ChecksumMismatch:
            return "crc32 mismatch";
        case FrameError::SequenceRegression:
            return "seq repeated or went backwards";
    }
    return "unknown";
}

void FrameReader::DropConsumed() {
    if (cursor_ == 0) {
        return;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(cursor_));
    cursor_ = 0;
}

}  // namespace atlas
