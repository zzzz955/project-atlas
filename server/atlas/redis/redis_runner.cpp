// =============================================================================
// RedisRunner::Submit 구현. 완료를 guard 로 감싸 executor 경계를 넘김
// =============================================================================

#include "atlas/redis/redis_runner.h"

#include <functional>
#include <utility>

#include "atlas/core/error.h"

namespace atlas
{

void RedisRunner::Submit( Ctx ctx, const RedisCommand& command, Completion completion,
                          CompletionPoster poster )
{
    connection_->Execute(
        ctx, command,
        [ctx, completion = std::move( completion ),
         poster = std::move( poster )]( const RedisResult& result )
        {
            if ( !completion )
            {
                return;
            }
            // [AD 11.2b] 양쪽 경로 모두 guard 통과
            // post 경로는 이 프레임이 사라진 뒤 남의 executor 위에 내려앉는다
            // 거기서 새는 예외는 I/O 스레드와 그것이 맡던 세션 전부를 죽임
            if ( poster )
            {
                poster( Guarded( ctx, [ctx, completion, result] { completion( ctx, result ); } ) );
                return;
            }
            Guarded( ctx, [&ctx, &completion, &result] { completion( ctx, result ); } )();
        } );
}

}  // namespace atlas
