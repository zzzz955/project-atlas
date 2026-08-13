#include "atlas/net/acceptor.h"

#include <string_view>
#include <tuple>
#include <utility>

#include "atlas/core/error.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"

namespace atlas {

SessionAcceptor::SessionAcceptor(IoContext& io_context, const Endpoint& endpoint,
                                 AcceptHandler on_accept, Duration session_idle_timeout)
    : io_context_(io_context),
      acceptor_(io_context),
      strand_(asio::make_strand(io_context)),
      retry_timer_(io_context),
      accept_handler_(std::move(on_accept)),
      session_idle_timeout_(session_idle_timeout) {
    acceptor_.open(endpoint.protocol());
    // Without this a restarted server cannot rebind a port still in TIME_WAIT.
    acceptor_.set_option(Acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
    local_endpoint_ = acceptor_.local_endpoint();
}

SessionAcceptor::~SessionAcceptor() = default;

void SessionAcceptor::Start() {
    asio::post(strand_, Guarded(ctx_, [this] { StartAcceptOnStrand(); }));
    ATLAS_LOG_INFO("acceptor listening on {}:{}", local_endpoint_.address().to_string(),
                   local_endpoint_.port());
}

void SessionAcceptor::Stop() {
    asio::post(strand_, Guarded(ctx_, [this] { CloseOnStrand(); }));
}

void SessionAcceptor::StartAcceptOnStrand() {
    if (closed_) {
        return;
    }

    // 🔴 The failure hook re-arms the loop. Without it a single throw out of the owner's accept
    // callback would leave the guard having logged an ERROR and the listener silently dead — a
    // total outage that looks like a healthy process, which is the failure shape §11.2(b) is about.
    acceptor_.async_accept(asio::bind_executor(
        strand_,
        Guarded(
            ctx_,
            [this](const ErrorCode& ec, Socket socket) { OnAcceptOnStrand(ec, std::move(socket)); },
            FailureHandler([this](std::string_view) { StartAcceptOnStrand(); }))));
}

void SessionAcceptor::OnAcceptOnStrand(const ErrorCode& ec, Socket socket) {
    if (ec) {
        // Stop() cancels the pending accept, and that cancellation arrives here. It is the normal
        // shutdown path, not a failure, and re-arming on it would resurrect the listener.
        if (ec == asio::error::operation_aborted) {
            return;
        }
        // Everything else is per-connection (a descriptor limit, a peer that vanished during the
        // handshake) and must not take the listener down with it. Re-armed after a delay rather
        // than immediately — see kAcceptRetryDelay for the failure that costs.
        ATLAS_LOG_WARN("accept failed: {}", ec.message());
        RetryAcceptOnStrand();
        return;
    }

    // 🔴 Nagle batches small writes and this is a latency-sensitive game connection, so it is off
    // from the first byte. Failure is not fatal — a connection with Nagle on still works, it is
    // only slower — so this warns and carries on.
    // The discard is written out for the reason CloseOnStrand states below: the sync overload
    // reports the failure through both the out-param and the return value.
    ErrorCode nodelay_ec;
    std::ignore = socket.set_option(Tcp::no_delay(true), nodelay_ec);
    if (nodelay_ec) {
        ATLAS_LOG_WARN("TCP_NODELAY not set: {}", nodelay_ec.message());
    }

    const SessionId id{next_session_id_++};
    auto session = Session::Create(io_context_, std::move(socket), id, session_idle_timeout_);
    if (accept_handler_) {
        accept_handler_(session);
    }
    // Started after the callback, so the owner's handlers are installed before the first byte can
    // arrive — the session's members belong to its strand from Start() onwards.
    session->Start();

    ATLAS_LOG_DEBUG("session {} accepted", IdValue(id));
    StartAcceptOnStrand();
}

void SessionAcceptor::RetryAcceptOnStrand() {
    if (closed_) {
        return;
    }

    retry_timer_.expires_after(kAcceptRetryDelay);
    retry_timer_.async_wait(asio::bind_executor(
        strand_,
        Guarded(
            ctx_,
            [this](const ErrorCode& timer_ec) {
                // Stop() cancels this wait, and re-arming on it would resurrect the listener — the
                // same reason OnAcceptOnStrand returns on operation_aborted.
                if (timer_ec == asio::error::operation_aborted) {
                    return;
                }
                StartAcceptOnStrand();
            },
            // The listener has to survive a throw here for the reason StartAcceptOnStrand states.
            FailureHandler([this](std::string_view) { StartAcceptOnStrand(); }))));
}

void SessionAcceptor::CloseOnStrand() {
    if (closed_) {
        return;
    }
    closed_ = true;

    // A delayed retry in flight would otherwise re-arm the accept loop after the shutdown.
    retry_timer_.cancel();

    // 🔴 Boost's sync ops report the failure TWICE — through the out-param and through the return
    // value — so passing `ignored` is only half of saying "dropped on purpose". The discard has to
    // be written out, or the return copy is dropped silently and the reader cannot tell deliberate
    // from forgotten.
    ErrorCode ignored;
    std::ignore = acceptor_.cancel(ignored);
    std::ignore = acceptor_.close(ignored);
    ATLAS_LOG_INFO("acceptor stopped on port {}", local_endpoint_.port());
}

}  // namespace atlas
