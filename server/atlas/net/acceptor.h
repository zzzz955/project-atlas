#pragma once
#include <functional>
#include <memory>

#include "atlas/core/ctx.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/net/net_types.h"
#include "atlas/net/session.h"

namespace atlas {

// architecture-design.md §5.1 — connection termination. Listens, accepts, turns each accepted
// socket into a Session, and starts it.
//
// The class is named `SessionAcceptor` rather than `Acceptor` because `atlas::Acceptor` is already
// the alias for `tcp::acceptor` (net_types.h); the two names would collide in the same namespace.
//
// 🔴 The accept loop runs on its own strand, so the connection counter below needs no lock. The
// object must outlive its pending accept: the shutdown order is
//     acceptor.Stop()  ->  runner.Stop()  ->  destroy
// (Stop posts the close onto the strand, so a runner that has already drained will never run it.)
class SessionAcceptor {
public:
    // Called on the accept strand, after the Session exists and before it is started, which is the
    // window in which the owner installs the session's own callbacks and takes ownership.
    using AcceptHandler = std::function<void(const std::shared_ptr<Session>&)>;

    // Opens, binds and listens immediately: a port that cannot be taken is a start-up failure, so
    // it throws here rather than reporting an error_code nobody would be positioned to handle.
    //
    // `session_idle_timeout` is handed to every session this acceptor creates. It is defaulted
    // because no server has a reason to override it — the tests do, and that is the whole point of
    // it being a parameter (Session::kDefaultIdleTimeout says why the value is what it is).
    SessionAcceptor(IoContext& io_context, const Endpoint& endpoint, AcceptHandler on_accept,
                    Duration session_idle_timeout = Session::kDefaultIdleTimeout);

    SessionAcceptor(const SessionAcceptor&) = delete;
    SessionAcceptor& operator=(const SessionAcceptor&) = delete;
    SessionAcceptor(SessionAcceptor&&) = delete;
    SessionAcceptor& operator=(SessionAcceptor&&) = delete;
    ~SessionAcceptor();

    void Start();

    // Stops accepting. Safe from any thread; the close itself happens on the accept strand.
    void Stop();

    // Captured once at construction rather than queried from the socket, so reading it from
    // another thread cannot race the accept loop. This is also how a caller learns the port the OS
    // picked when it asked for port 0.
    [[nodiscard]] const Endpoint& LocalEndpoint() const noexcept { return local_endpoint_; }

private:
    void StartAcceptOnStrand();
    void OnAcceptOnStrand(const ErrorCode& ec, Socket socket);
    void RetryAcceptOnStrand();
    void CloseOnStrand();

    // 🔴 An accept that fails because the process is out of descriptors (EMFILE/ENFILE) fails again
    // immediately, so re-arming with no delay turns a resource shortage into a WARN flood at 100%
    // CPU: the listener is alive and the process is unusable. A short fixed delay is the whole fix.
    //
    // 🔴 Deliberately NOT exponential. Growing the delay means carrying backoff state, and that
    // state is a second state machine in a class whose correctness argument is that it has one.
    // 100ms bounds the spin at ten attempts per second, which no longer starves anything.
    static constexpr Duration kAcceptRetryDelay = Millis{100};

    IoContext& io_context_;
    Acceptor acceptor_;
    Strand strand_;
    // Lives on the accept strand like everything else here, so it needs no lock of its own.
    SteadyTimer retry_timer_;
    Endpoint local_endpoint_;
    AcceptHandler accept_handler_;
    Duration session_idle_timeout_;
    Ctx ctx_;
    // architecture-design.md §6 — the session tier of the identity ladder. Handed out from the
    // accept strand, so a plain counter is correct here.
    UInt64 next_session_id_{1};
    bool closed_{false};
};

}  // namespace atlas
