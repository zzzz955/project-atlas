#pragma once

// =============================================================================
// [AD 10.2] Redis 커맨드 진입점. atlas/db 의 DbRunner 와 같은 분할이며
// 유일한 차이는 스레드 풀 대신 strand 라는 점
// =============================================================================

#include <functional>

#include "atlas/core/ctx.h"
#include "atlas/redis/connection.h"

namespace atlas
{

// [AD 9.2] 원장 교차 지점. Ctx 는 값으로 건넨다
// 참조면 호출자의 CtxScope 가 풀리는 순간 dangling 이 된다
// guard 가 사본을 실제 실행 지점에 다시 설치한다
class RedisRunner
{
public:
    // 커맨드 이후 poster 가 보낸 executor 위에서 실행
    // 결과에 관심이 없으면 비어 있어도 됨 - 캐시 쓰기 같은 경우
    using Completion = std::function< void( const Ctx&, const RedisResult& ) >;

    // 호출자 executor 로 돌아가는 길
    // 비어 있으면 응답이 떨어진 곳(연결의 strand)에서 완료 실행
    // [CS 6] 템플릿 인자 대신 type erasure
    // executor 마다의 인스턴스화 대신 런타임 간접 호출 1회
    using CompletionPoster = std::function< void( std::function< void() > ) >;

    explicit RedisRunner( RedisConnection& connection ) : connection_( &connection ) {}

    // 커맨드 1개 큐잉. DbRunner::Submit 과 달리 거부를 따로 보고하지 않는다
    // 정지나 불통을 ok == false 로 전달한다
    // 모든 호출자가 이미 처리해야 하는 답이라 두 번째 실패 경로는 잊히기만 함

    // command 가 const 참조인 이유는 드라이버가 인자를 복사하기 때문
    // 요청을 만들며 자기 직렬화 버퍼로 옮기므로 값 전달은 복사 1회를 더할 뿐
    void Submit( Ctx ctx, const RedisCommand& command, Completion completion = {},
                 CompletionPoster poster = {} );

    // RedisConnection::IsConfigured 전달
    // 캐시 미설정과 캐시 무응답이 같은 ok == false 로 온다
    // 경고할 가치가 있는 것은 한쪽뿐
    [[nodiscard]] bool IsConfigured() const noexcept { return connection_->IsConfigured(); }

private:
    // [CS 4.4] 비소유 관찰자
    // 연결이 runner 보다 오래 사는 것은 구성상 보장된다
    RedisConnection* connection_;
};

}  // namespace atlas
