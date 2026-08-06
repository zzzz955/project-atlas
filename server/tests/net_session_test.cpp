#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <future>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "atlas/core/error.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/net/acceptor.h"
#include "atlas/net/io_runner.h"
#include "atlas/net/net_types.h"
#include "atlas/net/session.h"

namespace {

using atlas::Byte;

std::vector<Byte> MakeBytes(std::string_view text) {
    std::vector<Byte> bytes;
    bytes.reserve(text.size());
    for (const char letter : text) {
        bytes.push_back(static_cast<Byte>(letter));
    }
    return bytes;
}

// Spins until `predicate` holds. Every wait in this file is bounded, so a regression shows up as a
// failed assertion instead of a hung CI job.
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

// A synchronous client on its own io_context, driven by the test thread: the assertions then read
// top to bottom with no completion handlers of their own.
class TestClient {
public:
    explicit TestClient(const atlas::Endpoint& endpoint) : socket_(io_context_) {
        socket_.connect(endpoint);
    }

    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;

    ~TestClient() { Disconnect(); }

    void Write(const std::vector<Byte>& bytes) {
        atlas::asio::write(socket_, atlas::asio::buffer(bytes));
    }

    std::vector<Byte> Read(std::size_t count) {
        std::vector<Byte> bytes(count);
        atlas::asio::read(socket_, atlas::asio::buffer(bytes));
        return bytes;
    }

    void Disconnect() {
        atlas::ErrorCode ignored;
        socket_.shutdown(atlas::Socket::shutdown_both, ignored);
        socket_.close(ignored);
    }

private:
    atlas::IoContext io_context_;
    atlas::Socket socket_;
};

// An echo server over the real acceptor / session / runner stack.
//
// 🔴 The echo is byte-for-byte: there is no framing in this layer, so the only thing a round trip
// can assert is that the same bytes came back.
class NetSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        for (std::size_t i = 0; i < kMaxTrackedSessions; ++i) {
            accepted_futures_[i] = accepted_[i].get_future();
        }

        // Two workers, so a strand that failed to serialise would actually be observable.
        runner_ = std::make_unique<atlas::IoRunner>(2);
        // 🔴 Loopback, not Tcp::v4(): binding the any-address makes local_endpoint() report
        // 0.0.0.0, and Windows rejects a connect() to that address with WSAEADDRNOTAVAIL. Port 0
        // still lets the OS pick a free port, so parallel ctest runs cannot collide.
        acceptor_ = std::make_unique<atlas::SessionAcceptor>(
            runner_->Context(), atlas::Endpoint(atlas::asio::ip::address_v4::loopback(), 0),
            [this](const std::shared_ptr<atlas::Session>& session) { OnAccept(session); });
        acceptor_->Start();
        runner_->Start();
    }

    void TearDown() override {
        // architecture-design.md §9 — the recorded shutdown order. Nothing else is reset here: the
        // members are destroyed in reverse declaration order, which releases the sessions held by
        // the futures before the io_context they belong to goes away.
        acceptor_->Stop();
        runner_->Stop();
    }

    void OnAccept(const std::shared_ptr<atlas::Session>& session) {
        session->SetBytesHandler(
            [](const std::shared_ptr<atlas::Session>& peer, std::span<const Byte> bytes) {
                // 🔴 The trigger for the containment test. A handler that throws must not take the
                // I/O thread with it (architecture-design.md §11.2b).
                if (!bytes.empty() && bytes.front() == static_cast<Byte>('X')) {
                    ATLAS_THROW(atlas::Exception, "deliberate handler failure");
                }
                peer->Send(bytes);
            });
        session->SetCloseHandler(
            [this](const std::shared_ptr<atlas::Session>&) { closed_count_.fetch_add(1); });

        // Runs on the accept strand, so the counter needs no synchronisation of its own.
        const std::size_t index = accepted_count_++;
        if (index < kMaxTrackedSessions) {
            accepted_[index].set_value(session);
        }
    }

    // One call per index: the future's value is moved out, which is what leaves the in-flight
    // handler as the only owner in the lifetime test.
    std::shared_ptr<atlas::Session> TakeSession(std::size_t index) {
        if (accepted_futures_[index].wait_for(atlas::Seconds{5}) != std::future_status::ready) {
            return nullptr;
        }
        return accepted_futures_[index].get();
    }

    static constexpr std::size_t kMaxTrackedSessions = 4;

    // 🔴 Declaration order is the destruction contract: runner_ is destroyed last, after the
    // futures have released the sessions whose sockets live on its io_context.
    std::unique_ptr<atlas::IoRunner> runner_;
    std::unique_ptr<atlas::SessionAcceptor> acceptor_;
    std::array<std::promise<std::shared_ptr<atlas::Session>>, kMaxTrackedSessions> accepted_;
    std::array<std::future<std::shared_ptr<atlas::Session>>, kMaxTrackedSessions> accepted_futures_;
    std::size_t accepted_count_{0};
    std::atomic<atlas::UInt32> closed_count_{0};
};

TEST_F(NetSessionTest, RoundTripsRawBytes) {
    TestClient client(acceptor_->LocalEndpoint());

    const std::vector<Byte> payload = MakeBytes("atlas raw byte stream");
    client.Write(payload);

    EXPECT_EQ(client.Read(payload.size()), payload);
}

// 🔴 architecture-design.md §9.1 — the whole reason session state carries no lock. If the strand
// ever stopped serialising, the maximum observed re-entrancy would exceed one and every unlocked
// member in Session would become a data race.
TEST_F(NetSessionTest, StrandSerialisesConcurrentPosts) {
    TestClient client(acceptor_->LocalEndpoint());
    const std::shared_ptr<atlas::Session> session = TakeSession(0);
    ASSERT_NE(session, nullptr);

    std::atomic<atlas::UInt32> in_flight{0};
    std::atomic<atlas::UInt32> max_in_flight{0};
    std::atomic<atlas::UInt32> completed{0};

    constexpr atlas::UInt32 kPosterThreads = 4;
    constexpr atlas::UInt32 kPostsPerThread = 250;

    std::vector<std::thread> posters;
    posters.reserve(kPosterThreads);
    for (atlas::UInt32 thread_index = 0; thread_index < kPosterThreads; ++thread_index) {
        posters.emplace_back([&] {
            for (atlas::UInt32 post_index = 0; post_index < kPostsPerThread; ++post_index) {
                atlas::asio::post(session->GetStrand(), [&] {
                    const atlas::UInt32 now = in_flight.fetch_add(1) + 1;
                    atlas::UInt32 observed = max_in_flight.load();
                    while (now > observed && !max_in_flight.compare_exchange_weak(observed, now)) {
                        // compare_exchange_weak refreshes `observed` on failure.
                    }
                    // Widens the window in which a broken strand would let a second handler in.
                    std::this_thread::yield();
                    in_flight.fetch_sub(1);
                    completed.fetch_add(1);
                });
            }
        });
    }
    for (std::thread& poster : posters) {
        poster.join();
    }

    ASSERT_TRUE(WaitUntil([&] { return completed.load() == kPosterThreads * kPostsPerThread; }));
    EXPECT_EQ(max_in_flight.load(), atlas::UInt32{1});
}

// 🔴 architecture-design.md §11.2(b) — the regression line. An exception escaping an asio handler
// reaches io_context::run() and kills that I/O thread, silencing every session it served while the
// process stays up. `Guarded` is what stops it, and IoRunner deliberately has no second net.
TEST_F(NetSessionTest, HandlerExceptionDoesNotKillTheIoThread) {
    const atlas::UInt64 errors_before = atlas::LogCount(atlas::LogLevel::Error);

    {
        TestClient thrower(acceptor_->LocalEndpoint());
        const std::shared_ptr<atlas::Session> session = TakeSession(0);
        ASSERT_NE(session, nullptr);

        thrower.Write(MakeBytes("X"));

        // The guard logs, then runs the injected failure hook, which closes the session.
        ASSERT_TRUE(WaitUntil([&] { return closed_count_.load() >= 1; }));
    }

    EXPECT_GE(atlas::LogCount(atlas::LogLevel::Error), errors_before + 1);

    // The proof that the pool survived: a brand new connection is still served.
    TestClient healthy(acceptor_->LocalEndpoint());
    const std::vector<Byte> payload = MakeBytes("still serving");
    healthy.Write(payload);
    EXPECT_EQ(healthy.Read(payload.size()), payload);
}

// cpp-style.md §4.4 — enable_shared_from_this is mandatory for asio sessions. This is what it buys:
// the owner can drop its reference while an operation is in flight and the session stays alive.
TEST_F(NetSessionTest, InFlightHandlerKeepsTheSessionAlive) {
    TestClient client(acceptor_->LocalEndpoint());

    std::weak_ptr<atlas::Session> weak;
    {
        const std::shared_ptr<atlas::Session> session = TakeSession(0);
        ASSERT_NE(session, nullptr);
        weak = session;
    }

    // The test holds nothing now; the pending async_read_some holds shared_from_this().
    EXPECT_FALSE(weak.expired());

    const std::vector<Byte> payload = MakeBytes("ping");
    client.Write(payload);
    EXPECT_EQ(client.Read(payload.size()), payload);
    EXPECT_FALSE(weak.expired());

    // And it does go away once the connection does.
    client.Disconnect();
    EXPECT_TRUE(WaitUntil([&] { return closed_count_.load() >= 1; }));
}

TEST(NetIoRunner, GracefulStopDrainsTheQueueAndIsIdempotent) {
    atlas::IoRunner runner;
    EXPECT_GT(runner.WorkerCount(), std::size_t{0});

    std::atomic<bool> ran{false};
    runner.Start();
    atlas::asio::post(runner.Context(), [&] { ran.store(true); });

    // Graceful means "release the work guard and let the queue finish", so the posted handler must
    // have run by the time Stop() returns.
    runner.Stop();
    EXPECT_TRUE(ran.load());

    runner.Stop();
}

TEST(NetIoRunner, ExplicitWorkerCountIsHonoured) {
    const atlas::IoRunner runner(3);
    EXPECT_EQ(runner.WorkerCount(), std::size_t{3});
}

}  // namespace
