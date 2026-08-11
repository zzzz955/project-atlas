#include "atlas/redis/redis_runner.h"

#include <functional>
#include <utility>

#include "atlas/core/error.h"

namespace atlas {

void RedisRunner::Submit(Ctx ctx, const RedisCommand& command, Completion completion,
                         CompletionPoster poster) {
    connection_->Execute(
        ctx, command,
        [ctx, completion = std::move(completion),
         poster = std::move(poster)](const RedisResult& result) {
            if (!completion) {
                return;
            }
            // 🔴 architecture-design.md 11.2b - both paths go through the guard. The inline one
            // already runs inside the connection's own guard, but the posted one lands on a
            // FOREIGN executor (a session strand) after this frame is gone, and an exception
            // escaping there kills an I/O thread and silences every session it served.
            if (poster) {
                poster(Guarded(ctx, [ctx, completion, result] { completion(ctx, result); }));
                return;
            }
            Guarded(ctx, [&ctx, &completion, &result] { completion(ctx, result); })();
        });
}

}  // namespace atlas
