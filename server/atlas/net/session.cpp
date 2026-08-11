#include "atlas/net/session.h"

#include <tuple>
#include <utility>

#include "atlas/core/log.h"

namespace atlas {

std::shared_ptr<Session> Session::Create(IoContext& io_context, Socket socket, SessionId id) {
    // std::make_shared cannot reach a private constructor, and a passkey type to work around that
    // would cost more than the one extra allocation this costs.
    return std::shared_ptr<Session>(new Session(io_context, std::move(socket), id));
}

Session::Session(IoContext& io_context, Socket socket, SessionId id)
    : socket_(std::move(socket)), strand_(asio::make_strand(io_context)), id_(id) {
    // The ledger travels with the session, so every log line and every exception raised inside a
    // guarded handler already carries the connection it belongs to (architecture-design.md §9.2).
    ctx_.session_id = id;
}

Session::~Session() = default;

void Session::SetBytesHandler(BytesHandler handler) { bytes_handler_ = std::move(handler); }

void Session::SetCloseHandler(CloseHandler handler) { close_handler_ = std::move(handler); }

void Session::Start() {
    auto self = shared_from_this();
    asio::post(strand_, Guarded(ctx_, [self] { self->StartReadOnStrand(); }, MakeFailureHandler()));
}

void Session::Send(std::span<const Byte> bytes) {
    // 🔴 Copied here, not at the write: `bytes` may name a temporary that is gone by the time the
    // strand runs the enqueue.
    std::vector<Byte> payload(bytes.begin(), bytes.end());
    auto self = shared_from_this();
    asio::post(strand_, Guarded(
                            ctx_,
                            [self, payload = std::move(payload)]() mutable {
                                self->EnqueueOnStrand(std::move(payload));
                            },
                            MakeFailureHandler()));
}

void Session::Close() {
    auto self = shared_from_this();
    asio::post(
        strand_,
        Guarded(ctx_, [self] { self->CloseOnStrand("closed by owner"); }, MakeFailureHandler()));
}

FailureHandler Session::MakeFailureHandler() {
    // Captures a strong reference: the guard runs after the handler body has already failed, and it
    // still has to be able to shut the connection down.
    return {[self = shared_from_this()](std::string_view reason) {
        // Already on the strand — `Guarded::Fail` runs inside the handler it wrapped.
        self->CloseOnStrand(reason);
    }};
}

void Session::StartReadOnStrand() {
    if (closed_) {
        return;
    }

    auto self = shared_from_this();
    socket_.async_read_some(
        asio::buffer(read_buffer_),
        asio::bind_executor(strand_, Guarded(
                                         ctx_,
                                         [self](const ErrorCode& ec, std::size_t bytes_read) {
                                             self->OnReadOnStrand(ec, bytes_read);
                                         },
                                         MakeFailureHandler())));
}

void Session::OnReadOnStrand(const ErrorCode& ec, std::size_t bytes_read) {
    if (ec) {
        // architecture-design.md §11.2(a) — a peer going away is an expected outcome, so it arrives
        // as an error_code and not as an exception.
        CloseOnStrand(ec.message());
        return;
    }

    if (bytes_handler_) {
        // 🔴 Raw bytes, exactly as the socket produced them. Nothing here interprets the contents;
        // that belongs to the frame layer of §8, which does not exist yet.
        bytes_handler_(shared_from_this(), std::span<const Byte>(read_buffer_.data(), bytes_read));
    }

    StartReadOnStrand();
}

void Session::EnqueueOnStrand(std::vector<Byte>&& payload) {
    if (closed_ || payload.empty()) {
        return;
    }

    // 🔴 architecture-design.md §9 — back-pressure. Written as a subtraction against the cap rather
    // than `write_queue_bytes_ + payload.size() > kMaxWriteQueueBytes` so the sum cannot overflow;
    // the invariant `write_queue_bytes_ <= kMaxWriteQueueBytes` makes the left side safe.
    // The connection is closed, not the payload dropped — a drop breaks the protocol quietly.
    if (payload.size() > kMaxWriteQueueBytes - write_queue_bytes_) {
        CloseOnStrand("write queue above kMaxWriteQueueBytes");
        return;
    }

    // An empty queue means no write is in flight, so this one has to start the chain. A non-empty
    // one means OnWriteOnStrand will pick the new entry up when the current write completes.
    const bool idle = write_queue_.empty();
    write_queue_bytes_ += payload.size();
    write_queue_.push_back(std::move(payload));
    if (idle) {
        StartWriteOnStrand();
    }
}

void Session::StartWriteOnStrand() {
    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(write_queue_.front()),
                      asio::bind_executor(strand_, Guarded(
                                                       ctx_,
                                                       [self](const ErrorCode& ec, std::size_t) {
                                                           self->OnWriteOnStrand(ec);
                                                       },
                                                       MakeFailureHandler())));
}

void Session::OnWriteOnStrand(const ErrorCode& ec) {
    if (ec) {
        CloseOnStrand(ec.message());
        return;
    }

    // 🔴 A successful write and a failed read can complete in the same strand batch, and the read
    // runs first: by the time this handler is entered the close has already happened. Without this
    // guard the queue below is walked on a session that has nothing left to send.
    if (closed_) {
        return;
    }

    write_queue_bytes_ -= write_queue_.front().size();
    write_queue_.pop_front();
    if (!write_queue_.empty()) {
        StartWriteOnStrand();
    }
}

// `reason` feeds a DEBUG line only, and DEBUG is compiled out of release builds (core/log.h), so
// the attribute is what keeps a release build warning-free without a cast at the use site.
void Session::CloseOnStrand([[maybe_unused]] std::string_view reason) {
    if (closed_) {
        return;
    }
    closed_ = true;

    // The error_code overloads: shutting down a socket the peer already dropped fails, and that is
    // not something the close path can or should react to.
    // The return value carries the same failure a second time; acceptor.cpp says why it is
    // discarded explicitly rather than dropped.
    ErrorCode ignored;
    std::ignore = socket_.shutdown(Socket::shutdown_both, ignored);
    std::ignore = socket_.close(ignored);

    // 🔴 The queue is deliberately NOT cleared here. `close()` cancels the in-flight `async_write`,
    // but cancellation is asynchronous: the buffer at the front stays registered with the OS until
    // the completion handler actually runs, so freeing it now is a use-after-free window. The
    // handler holds `shared_from_this()`, so the session — and with it the queue — outlives every
    // pending operation, and the memory is released when the last handler drops its reference.
    // `closed_` is what stops anything from being added or consumed in the meantime.

    ATLAS_LOG_DEBUG("session {} closed: {}", IdValue(id_), reason);

    if (close_handler_) {
        // Moved out first so the notification cannot fire twice even if the callback re-enters.
        const CloseHandler handler = std::move(close_handler_);
        close_handler_ = nullptr;
        handler(shared_from_this());
    }
}

}  // namespace atlas
