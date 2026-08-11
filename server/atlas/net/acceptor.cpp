#include "atlas/net/acceptor.h"

#include <string_view>
#include <tuple>
#include <utility>

#include "atlas/core/error.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"

namespace atlas {

SessionAcceptor::SessionAcceptor(IoContext& io_context, const Endpoint& endpoint,
                                 AcceptHandler on_accept)
    : io_context_(io_context),
      acceptor_(io_context),
      strand_(asio::make_strand(io_context)),
      accept_handler_(std::move(on_accept)) {
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
        // handshake) and must not take the listener down with it.
        ATLAS_LOG_WARN("accept failed: {}", ec.message());
        StartAcceptOnStrand();
        return;
    }

    const SessionId id{next_session_id_++};
    auto session = Session::Create(io_context_, std::move(socket), id);
    if (accept_handler_) {
        accept_handler_(session);
    }
    // Started after the callback, so the owner's handlers are installed before the first byte can
    // arrive — the session's members belong to its strand from Start() onwards.
    session->Start();

    ATLAS_LOG_DEBUG("session {} accepted", IdValue(id));
    StartAcceptOnStrand();
}

void SessionAcceptor::CloseOnStrand() {
    if (closed_) {
        return;
    }
    closed_ = true;

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
