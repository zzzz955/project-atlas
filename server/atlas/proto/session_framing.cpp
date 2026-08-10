#include "atlas/proto/session_framing.h"

#include <utility>
#include <vector>

#include "atlas/core/ids.h"
#include "atlas/core/log.h"
#include "atlas/net/session.h"
#include "atlas/proto/frame_reader.h"

namespace atlas {

void AttachFrameReader(const std::shared_ptr<Session>& session, FrameHandler on_frame) {
    // A shared_ptr rather than a mutable by-value capture: std::function requires the callable to
    // be copyable, and a copied lambda would silently carry a second reassembly buffer that then
    // falls behind the stream. One reader, one session.
    session->SetBytesHandler(
        [reader = std::make_shared<FrameReader>(), handler = std::move(on_frame)](
            const std::shared_ptr<Session>& source, std::span<const Byte> bytes) {
            std::vector<Frame> frames;
            const FrameError error = reader->Feed(bytes, frames);

            for (const Frame& frame : frames) {
                if (handler) {
                    handler(source, frame);
                }
            }

            if (error != FrameError::None) {
                ATLAS_LOG_WARN("session {} framing error: {}", IdValue(source->Id()),
                               FrameReader::Describe(error));
                source->Close();
            }
        });
}

bool SendFrame(Session& session, UInt16 opcode, UInt32 seq, std::span<const Byte> payload) {
    std::vector<Byte> bytes;
    if (!EncodeFrame(bytes, opcode, seq, payload)) {
        return false;
    }
    session.Send(bytes);
    return true;
}

}  // namespace atlas
