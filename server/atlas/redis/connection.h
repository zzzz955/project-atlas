#pragma once

// =============================================================================
// [AD 10.2] 캐시 클라이언트 계층의 우산 헤더
// 접속 설정, 커맨드/응답 어휘, 연결 객체를 함께 담음
// =============================================================================

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/net/net_types.h"

// =============================================================================
// 계층 규약
// =============================================================================

// [AD 15.4] 이 계층은 게임을 모름
// core-purity 가 generated 트리 include 와 게임 어휘를 금지한다
// 키 형식도 수명 정책도 전부 위 계층의 몫
// [AD 10.3] atlas/db 가 DbValue 로 긋는 것과 같은 seam

// [AD 10.2] pub/sub API 없음
// [AD 5.2] WORLD 직접 연결 금지를 Redis 채널이 우회로로 뚫어 준다
// 글자만 지키고 뜻을 깨면 Phase 3 Relay 자리가 사라진다
// 방지선은 그 커맨드 이름 grep 이 비는 것이라 주석에도 적지 않는다

// [AD 10.3] atlas/db 와 달리 전용 스레드 풀 없음
// libmariadb 는 동기라 질의가 스레드를 막는다
// boost-redis 는 asio 네이티브라 스레드를 양보한다
// [AD 9.1] 필요한 것은 직렬화뿐이고 답은 strand 하나

// 재시도, 백오프, 서킷 브레이커 없음
// 캐시는 사본이고 진실은 DB 에 있다
// 무응답의 정답은 기다림이 아니라 DB 조회

// 범위 밖: cluster, sentinel, 파이프라이닝 튜닝, Lua 스크립팅

namespace atlas
{

// [AD 5.4] 모든 필드의 출처는 .env 의 SecretConfig
// server.ini 에 두지 않고 로그나 예외 메시지에도 넣지 않는다
struct RedisConnectionConfig
{
    std::string host{};
    UInt16 port{ 6379 };
    std::string password{};
};

// 커맨드 1개 = 동사 + 문자열화가 끝난 인자들
// variant 가 아닌 이유는 RESP 가 bulk string 만 말하기 때문
// 값의 의미를 아는 호출자가 포맷하는 것이 옳다
struct RedisCommand
{
    std::string verb{};
    std::vector< std::string > args{};
};

// 응답 1개를 깊이 우선 평탄화. nullopt 는 RESP null, 집합 헤더는 기여 없음.
// ZREVRANGE WITHSCORES 는 member, score, member, score 순으로 도착
using RedisReply = std::vector< std::optional< std::string > >;

struct RedisResult
{
    // false = 커맨드 미완료(연결 없음, 취소, 서버 오류).
    // "키가 없음"이 아님. 그것은 null 을 담은 성공 응답
    bool ok{ false };
    RedisReply values{};
};

// io_context 위에 얹히는 다중화 Redis 연결 1개
// [AD 9.1] 모든 연산은 strand 하나에 dispatch
// boost::redis::connection 은 스레드 안전하지 않다
// 동시성 모델도 strand 하나뿐이라 이 클래스에 mutex 없음
class RedisConnection
{
public:
    // 커맨드가 완료 또는 실패한 뒤 이 연결의 strand 위에서 호출
    using Handler = std::function< void( const RedisResult& ) >;

    RedisConnection( IoContext& io_context, const RedisConnectionConfig& config );
    ~RedisConnection();

    RedisConnection( const RedisConnection& ) = delete;
    RedisConnection& operator=( const RedisConnection& ) = delete;
    RedisConnection( RedisConnection&& ) = delete;
    RedisConnection& operator=( RedisConnection&& ) = delete;

    // io_context 위에서 접속/재접속 루프 시작
    // 논블로킹이며 서버가 닿지 않아도 실패하지 않는다
    // 여기서 실패시키면 Redis 다운이 게임 서버 다운이 된다
    void Start();

    // IoRunner::Stop() 보다 먼저 호출할 것. 멱등
    // 실행 루프가 io_context 의 미완 작업이라 순서가 중요하다
    // work guard 가 먼저 풀리면 루프는 영영 배수되지 않는다
    void Stop() noexcept;

    // PING 왕복 또는 마감까지 블로킹. 기동 스레드와 테스트 전용
    // I/O 스레드에서 호출하면 자기 자신을 기다리게 된다
    [[nodiscard]] bool WaitUntilReady( Duration timeout );

    // 생성 시 host 가 주어졌는지 여부
    // 서버가 살아있는가가 아니다. 설정된 서버가 죽어 있어도 true
    // [AD 10.2] 캐시는 선택이고 부재는 경고 수준
    // 이걸 먼저 묻지 않으면 캐시를 껐다가 캐시가 고장났다로 매 요청 기록된다
    [[nodiscard]] bool IsConfigured() const noexcept;

    // 커맨드 1개 큐잉. handler 는 정확히 1회 실행된다
    // 오지 않을 답을 영영 기다리는 호출자가 없다
    // 정지 상태는 즉시 거부, 무응답은 .cpp 의 마감이 제한
    void Execute( Ctx ctx, const RedisCommand& command, Handler handler );

private:
    // pimpl: boost/redis 헤더를 이 라이브러리 소비자에게 감추는 기계적 절반.
    // 나머지 절반은 CMakeLists 의 PRIVATE 링크

    // shared_ptr 인 이유는 teardown
    // Stop() 이 취소를 strand 에 post 하므로 이 객체가 먼저 사라질 수 있다
    // I/O 스레드가 붙든 마지막 참조가 impl 을 파괴한다
    struct Impl;
    std::shared_ptr< Impl > impl_;
};

}  // namespace atlas
