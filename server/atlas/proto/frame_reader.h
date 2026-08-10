#pragma once
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "atlas/core/types.h"
#include "atlas/proto/frame.h"

namespace atlas {

// Why a frame is rejected. architecture-design.md §8.1 / §8.3 — every value other than `None` means
// the connection is closed. 🔴 Not dropped: a silent drop breaks the protocol quietly and the
// symptom surfaces days later somewhere unrelated.
enum class FrameError : UInt8 {
    None = 0,
    PayloadTooLarge,     // declared length above kMaxPayload
    ChecksumMismatch,    // §8.1 crc32 disagrees — framing desynchronised or bytes are corrupt
    SequenceRegression,  // §8.3 seq went backwards or repeated
};

// Reassembles the byte stream a Session hands over into whole frames.
//
// 🔴 Nothing here throws (architecture-design.md §11.2a). A truncated or hostile packet is ordinary
// input on a public socket, not an exceptional condition, so failures come back as a return value.
//
// 🔴 Not thread-safe, and it does not need to be: one reader belongs to one session and is only
// ever touched from that session's strand (§9.1). Adding a lock here would be a second concurrency
// model.
class FrameReader {
public:
    // Appends `bytes` to whatever was left over and extracts every frame that is now complete,
    // in arrival order, onto the back of `out`.
    //
    // Returns FrameError::None when the stream is still healthy — including the ordinary case where
    // nothing completed yet and the partial frame stays buffered. Any other value means the caller
    // must close the connection; frames decoded before the offending one are still appended to
    // `out`, so a caller may deliver them first. 🔴 A reader that has returned an error must not be
    // fed again — its buffer is deliberately left as-is at the point of failure.
    FrameError Feed(std::span<const Byte> bytes, std::vector<Frame>& out);

    // Bytes held back waiting for the rest of their frame. This is bounded by
    // kFrameHeaderSize + kMaxPayload plus one socket read, because a declared length above the cap
    // is rejected before anything is reserved for it.
    [[nodiscard]] std::size_t Pending() const noexcept;

    [[nodiscard]] static std::string_view Describe(FrameError error) noexcept;

private:
    // Compacts the consumed prefix away. Called once per Feed rather than once per frame, so a read
    // carrying several frames does not pay a memmove for each of them.
    void DropConsumed();

    std::vector<Byte> buffer_;
    std::size_t cursor_{0};
    UInt32 last_seq_{0};
    bool has_last_seq_{false};
};

}  // namespace atlas
