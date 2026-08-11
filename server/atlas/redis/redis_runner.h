#pragma once
#include <functional>

#include "atlas/core/ctx.h"
#include "atlas/redis/connection.h"

namespace atlas {

// architecture-design.md 10.2 - the command entry point, and the ledger's crossing (9.2).
//
// The split mirrors atlas/db exactly: RedisConnection owns the socket and the strand the way
// ConnectionPool owns the handles, and this type is where a request's Ctx is carried across the
// boundary and where the completion is handed back to whatever executor asked. Reading the two
// layers side by side is the point - the same shape twice makes the ONE difference between them
// (a thread pool for the blocking client, a strand for the asio-native one) visible instead of
// buried.
//
// 🔴 The Ctx crosses BY VALUE. A reference would dangle the moment the caller's CtxScope unwound
// on the I/O thread, which is the concrete trap 9.2 warns about; the guard re-installs the copy
// wherever the work actually lands.
class RedisRunner {
public:
    // Runs after the command, on whatever executor `poster` sends it to. May be empty when the
    // caller genuinely does not care about the outcome - a cache write, for instance.
    using Completion = std::function<void(const Ctx&, const RedisResult&)>;

    // How to get back to the caller's executor - typically
    // `[strand](std::function<void()> fn) { asio::post(strand, std::move(fn)); }`.
    //
    // 🔴 Type-erased rather than a template parameter, the same trade atlas/db takes: one indirect
    // call at run time against a whole instantiation per executor at compile time (cpp-style.md 6).
    // Empty means "run the completion where the reply landed", which is the connection's strand.
    using CompletionPoster = std::function<void(std::function<void()>)>;

    explicit RedisRunner(RedisConnection& connection) : connection_(&connection) {}

    // Queues one command. 🔴 Unlike DbRunner::Submit there is no refusal to report: a stopped or
    // unreachable server is delivered to the completion as `RedisResult::ok == false`, because
    // every caller of this layer already has to handle that answer and a second failure channel
    // would only invite one of them to be forgotten.
    //
    // 🔴 The command is a const reference, not a by-value sink. The driver copies the arguments
    // into its own serialised payload the moment the request is built, so a by-value parameter here
    // would only add a copy on the way in — and `std::move` into it would be the kind of move that
    // does nothing, which reads like an optimisation and is not one.
    void Submit(Ctx ctx, const RedisCommand& command, Completion completion = {},
                CompletionPoster poster = {});

private:
    // Non-owning observer: the connection outlives the runner by construction (cpp-style.md 4.4).
    RedisConnection* connection_;
};

}  // namespace atlas
