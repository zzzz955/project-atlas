#include "atlas/redis/connection.h"

#include <atomic>
#include <boost/asio/cancel_after.hpp>
#include <boost/redis/config.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/logger.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/resp3/node.hpp>
#include <boost/redis/resp3/tree.hpp>
#include <boost/redis/resp3/type.hpp>
#include <boost/redis/response.hpp>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "atlas/core/error.h"
#include "atlas/core/log.h"

namespace atlas {
namespace {

namespace redis = boost::redis;

// The library talks to us instead of to stderr. 🔴 Only `err` and above is forwarded: the
// reconnect loop is deliberately talkative and an INFO line per retry would bury the one message
// that matters when the cache is genuinely gone.
void ForwardRedisLog(redis::logger::level /*level*/, std::string_view message) {
    ATLAS_LOG_WARN("redis: {}", message);
}

// Flattens the RESP3 reply tree. Aggregate headers (array / map / set / push) carry no value of
// their own, so they contribute nothing and the leaves arrive in wire order.
RedisReply FlattenReply(const redis::generic_response& response) {
    RedisReply values;
    if (!response.has_value()) {
        return values;
    }
    const redis::resp3::tree& nodes = response.value();
    values.reserve(nodes.size());
    for (const redis::resp3::node& entry : nodes) {
        if (redis::resp3::is_aggregate(entry.data_type)) {
            continue;
        }
        if (entry.data_type == redis::resp3::type::null) {
            values.emplace_back(std::nullopt);
            continue;
        }
        values.emplace_back(entry.value);
    }
    return values;
}

// One in-flight command. Both halves must outlive the asynchronous operation, which is the whole
// reason this exists rather than two locals.
struct ExecState {
    redis::request request{};
    redis::generic_response response{};
};

// The time WaitUntilReady leaves one probe outstanding before trying again.
constexpr Millis kProbeBudget{500};
constexpr Millis kProbePause{50};

// The time Stop() waits for the run loop to finish. 🔴 Not a politeness delay: see Stop().
constexpr Seconds kStopBudget{5};

// 🔴 THE DEADLINE ON ONE COMMAND, AND THE REASON THE WHOLE FALLBACK STORY WORKS. Measured
// 2026-08-11 against a closed port: boost-redis 1.91 keeps an exec QUEUED while its run loop
// reconnects, so a lookup issued with the server down never completes on its own - the request
// that asked the cache a question waits forever and the client sees a server that stopped
// answering. `request::config::cancel_if_not_connected` is documented as deprecated in this
// version and did not produce the immediate failure either; the library's own replacement is
// asio::cancel_after, so that is what bounds it.
//
// The cost is stated: with Redis down every lookup pays this before going to the database. That is
// the same shape as the connection pool's acquire timeout (§10.3) - a bounded wait then a clear
// failure, never an unbounded one.
constexpr Millis kCommandBudget{500};

}  // namespace

struct RedisConnection::Impl {
    Impl(IoContext& io_context, const RedisConnectionConfig& config)
        : strand(asio::make_strand(io_context)),
          connection(strand, redis::logger{redis::logger::level::err, &ForwardRedisLog}) {
        run_config.addr.host = config.host;
        run_config.addr.port = std::to_string(config.port);
        run_config.password = config.password;
        finished_ready = finished.get_future().share();
    }

    Strand strand;
    redis::config run_config{};
    redis::connection connection;
    std::atomic<bool> running{false};

    // Fulfilled by the async_run completion. 🔴 This is what lets Stop() be a real stop rather
    // than a request for one - see Stop() for why "the loop has ended" has to be observable.
    std::promise<void> finished{};
    std::shared_future<void> finished_ready{};
};

RedisConnection::RedisConnection(IoContext& io_context, const RedisConnectionConfig& config)
    : impl_(std::make_shared<Impl>(io_context, config)) {}

RedisConnection::~RedisConnection() { Stop(); }

void RedisConnection::Start() {
    if (impl_->running.exchange(true)) {
        return;
    }
    // 🔴 Initiated ON the strand, not from here: async_run touches the connection's internals and
    // this call comes from the process lifetime thread. Everything that reaches the connection
    // goes through the one strand, which is what makes the absence of a mutex correct (9.1).
    std::shared_ptr<Impl> impl = impl_;
    asio::post(impl->strand, Guarded(Ctx{}, [impl] {
                   impl->connection.async_run(
                       impl->run_config, Guarded(Ctx{}, [impl](const ErrorCode& failure) {
                           // Reached only when the loop is cancelled or gives up for good; the
                           // reconnect wait keeps ordinary outages inside async_run.
                           ATLAS_LOG_INFO("redis run loop finished: {}", failure.message());
                           impl->finished.set_value();
                       }));
               }));
}

void RedisConnection::Stop() noexcept {
    if (!impl_->running.exchange(false)) {
        return;
    }
    try {
        // The strand keeps its own reference, so the cancellation is safe even when this object
        // is already gone by the time an I/O thread gets to it.
        std::shared_ptr<Impl> impl = impl_;
        asio::post(impl->strand, Guarded(Ctx{}, [impl] { impl->connection.cancel(); }));
        // 🔴 THEN WAIT FOR IT. IoRunner::Stop() only releases the work guard and joins, so a run
        // loop still outstanding at that moment makes the join wait forever - the whole process
        // hangs at shutdown with nothing in the log. Stopping has to be observable, not requested.
        if (impl->finished_ready.wait_for(kStopBudget) != std::future_status::ready) {
            ATLAS_LOG_ERROR("redis run loop did not finish within {}s of being cancelled",
                            kStopBudget.count());
        }
    } catch (...) {  // NOLINT - a noexcept teardown has nowhere left to report to.
    }
}

bool RedisConnection::WaitUntilReady(Duration timeout) {
    const TimePoint deadline = Clock::now() + timeout;
    while (true) {
        auto answered = std::make_shared<std::promise<bool>>();
        std::future<bool> probe = answered->get_future();
        Execute(Ctx{}, RedisCommand{.verb = "PING", .args = {}},
                [answered](const RedisResult& result) { answered->set_value(result.ok); });

        if (probe.wait_for(kProbeBudget) == std::future_status::ready && probe.get()) {
            return true;
        }
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(kProbePause);
    }
}

void RedisConnection::Execute(Ctx ctx, const RedisCommand& command, Handler handler) {
    if (!impl_->running.load()) {
        // Refused inline rather than queued. 🔴 The caller gets exactly one outcome either way;
        // silence is the one answer a read-through cache cannot recover from.
        handler(RedisResult{});
        return;
    }

    auto state = std::make_shared<ExecState>();
    if (command.args.empty()) {
        state->request.push(command.verb);
    } else {
        state->request.push_range(command.verb, command.args);
    }

    std::shared_ptr<Impl> impl = impl_;
    asio::post(impl->strand, Guarded(ctx, [impl, state, ctx, handler = std::move(handler)] {
                   impl->connection.async_exec(
                       state->request, state->response,
                       asio::cancel_after(
                           kCommandBudget,
                           Guarded(ctx, [state, handler](const ErrorCode& failure, std::size_t) {
                               RedisResult result;
                               result.ok = !failure && state->response.has_value();
                               if (result.ok) {
                                   result.values = FlattenReply(state->response);
                               }
                               handler(result);
                           })));
               }));
}

}  // namespace atlas
