#pragma once
#include <functional>
#include <memory>
#include <span>

#include "atlas/core/types.h"
#include "atlas/proto/frame.h"

namespace atlas {

// 🔴 Forward declaration on purpose: including atlas/net/session.h here would pull
// <boost/asio.hpp> into every consumer of the frame layer, and the three parsing headers of this
// library are deliberately asio-free.
class Session;

// The seam between the byte boundary and the game. architecture-design.md §8.1 / §14 — the core
// hands over `(session, frame)` and knows nothing further; the opcode -> handler table is
// game-side.
using FrameHandler = std::function<void(const std::shared_ptr<Session>&, const Frame&)>;

// Installs a FrameReader as the session's bytes handler.
//
// 🔴 Call before Session::Start(), like any other handler installation: afterwards the session's
// members belong to its strand.
//
// The reader is owned by the handler the session holds, which makes the session its owner and the
// session strand its only accessor (§9.1) — no lock is added anywhere on this path. A framing error
// logs and closes the connection (§8.1): frames that decoded before the bad one are delivered
// first.
void AttachFrameReader(const std::shared_ptr<Session>& session, FrameHandler on_frame);

// Encodes one frame and queues it on the session's write path. Returns false without queuing
// anything when the payload exceeds kMaxPayload.
//
// 🔴 `seq` is supplied by the caller. §8.3 keeps one send counter per session, and the layer that
// decides what to send is the layer that owns it; a counter hidden in this free function would be
// touched off-strand by whoever called Send, which is exactly the race the strand model forbids.
bool SendFrame(Session& session, UInt16 opcode, UInt32 seq, std::span<const Byte> payload);

}  // namespace atlas
