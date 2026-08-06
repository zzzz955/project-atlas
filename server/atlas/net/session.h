#pragma once
#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/error.h"
#include "atlas/core/ids.h"
#include "atlas/core/types.h"
#include "atlas/net/net_types.h"

namespace atlas {

// architecture-design.md §9 — one TCP connection, serialised by its own strand.
//
// 🔴 THE BOUNDARY OF THIS CLASS IS BYTES. It reads a byte stream and writes a byte stream. Frame
// headers, payload identifiers, ordering counters and integrity fields are the protocol layer
// (§8), whose contract is still open (§16), and inventing a header here would have to be undone
// wholesale later. What arrives at `BytesHandler` is whatever the socket produced — it is not a
// message, and it is not guaranteed to be a whole anything.
//
// 🔴 Concurrency is the strand and nothing else (§9.1). Every member below is touched only from
// handlers bound to `strand_`, which is why there is not a single lock in this class. Putting one
// here would create a second concurrency model, and deadlocks live exactly at the crossing point.
class Session : public std::enable_shared_from_this<Session> {
public:
    // The session hands itself to the callback so the owner can answer on the same connection
    // without keeping its own map from raw pointer back to shared_ptr.
    using BytesHandler =
        std::function<void(const std::shared_ptr<Session>&, std::span<const Byte>)>;
    using CloseHandler = std::function<void(const std::shared_ptr<Session>&)>;

    // 🔴 cpp-style.md §4.4 — a session MUST be owned by a shared_ptr, because `shared_from_this()`
    // inside every handler is what keeps it alive while an operation is in flight. The constructor
    // is private so the stack-allocated mistake is a compile error rather than a comment.
    static std::shared_ptr<Session> Create(IoContext& io_context, Socket socket, SessionId id);

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session();

    // 🔴 Install the callbacks BEFORE Start(): afterwards the members belong to the strand and a
    // setter call from the owning thread would be a data race.
    void SetBytesHandler(BytesHandler handler);
    void SetCloseHandler(CloseHandler handler);

    // Begins the read loop. Safe to call from any thread — it posts to the strand.
    void Start();

    // Queues `bytes` for transmission. Safe to call from any thread. The buffer is copied before
    // the call returns, so the caller may hand over a temporary.
    void Send(std::span<const Byte> bytes);

    // Closes the connection. Safe to call from any thread, and idempotent.
    void Close();

    [[nodiscard]] SessionId Id() const noexcept { return id_; }

    // The strand every handler for this connection runs on. The owner posts its own work here to
    // inherit the same serialisation instead of adding a lock of its own.
    [[nodiscard]] Strand& GetStrand() noexcept { return strand_; }

private:
    Session(IoContext& io_context, Socket socket, SessionId id);

    // Everything below runs on the strand.
    void StartReadOnStrand();
    void OnReadOnStrand(const ErrorCode& ec, std::size_t bytes_read);
    void EnqueueOnStrand(std::vector<Byte>&& payload);
    void StartWriteOnStrand();
    void OnWriteOnStrand(const ErrorCode& ec);
    void CloseOnStrand(std::string_view reason);

    // The "close this connection safely" callback that core/error.h left as an injection point:
    // when `Guarded` swallows a throw, this is what takes the connection down.
    [[nodiscard]] FailureHandler MakeFailureHandler();

    // One socket read at a time, into a fixed buffer. Sizing this is not a protocol decision — it
    // is how much the kernel may hand over per completion.
    static constexpr std::size_t kReadBufferSize = 4096;

    Socket socket_;
    Strand strand_;
    SessionId id_;
    Ctx ctx_;

    std::array<Byte, kReadBufferSize> read_buffer_{};

    // Write policy (architecture-design.md §9): FIFO, exactly one `async_write` in flight, the next
    // one started from the completion of the previous. The strand is what makes an unlocked
    // container correct here. 🔴 Unbounded on purpose in this slice — a back-pressure cap needs a
    // message size to be meaningful, and messages do not exist until the frame layer does.
    std::deque<std::vector<Byte>> write_queue_;

    BytesHandler bytes_handler_;
    CloseHandler close_handler_;
    bool closed_{false};
};

}  // namespace atlas
