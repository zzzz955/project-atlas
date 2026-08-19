// =============================================================================
// RedisConnection 구현. boost-redis 드라이버와 strand 위의 실행 루프
// =============================================================================

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

namespace atlas
{
namespace
{

namespace redis = boost::redis;

// 라이브러리 로그를 stderr 대신 우리 로거로. err 이상만 전달
// 재접속 루프가 수다스러워 retry 마다 INFO 를 남기면 중요한 한 줄이 묻힘
void ForwardRedisLog( redis::logger::level /*level*/, std::string_view message )
{
    ATLAS_LOG_WARN( "redis: {}", message );
}

// RESP3 응답 트리 평탄화. 집합 헤더는 자체 값이 없어 잎만 와이어 순서로 남음
RedisReply FlattenReply( const redis::generic_response& response )
{
    RedisReply values;
    if ( !response.has_value() )
    {
        return values;
    }
    const redis::resp3::tree& nodes = response.value();
    values.reserve( nodes.size() );
    for ( const redis::resp3::node& entry : nodes )
    {
        if ( redis::resp3::is_aggregate( entry.data_type ) )
        {
            continue;
        }
        if ( entry.data_type == redis::resp3::type::null )
        {
            values.emplace_back( std::nullopt );
            continue;
        }
        values.emplace_back( entry.value );
    }
    return values;
}

// 진행 중인 커맨드 1개
// 두 멤버 모두 비동기 연산보다 오래 살아야 한다
// 지역 변수 둘이 아니라 이 구조체가 존재하는 이유
struct ExecState
{
    redis::request request{};
    redis::generic_response response{};
};

// WaitUntilReady 가 프로브 1회를 물고 기다리는 시간
constexpr Millis kProbeBudget{ 500 };
constexpr Millis kProbePause{ 50 };

// Stop() 이 실행 루프 종료를 기다리는 시간. 예의상 지연이 아님 - Stop() 참고
constexpr Seconds kStopBudget{ 5 };

// 커맨드 1개의 마감. 2026-08-11 닫힌 포트 실측
// boost-redis 1.91 은 재접속 중 exec 를 QUEUED 로 붙든다
// 서버가 죽으면 조회가 영영 끝나지 않는다
// [AD 10.3] 폐기 예정인 cancel_if_not_connected 대신 풀의 acquire 를 본떠 제한
constexpr Millis kCommandBudget{ 500 };  // 다운 시 조회마다 지불한 뒤 DB 폴백

}  // namespace

struct RedisConnection::Impl
{
    Impl( IoContext& io_context, const RedisConnectionConfig& config )
        : strand( asio::make_strand( io_context ) ),
          connection( strand, redis::logger{ redis::logger::level::err, &ForwardRedisLog } )
    {
        run_config.addr.host = config.host;
        run_config.addr.port = std::to_string( config.port );
        run_config.password = config.password;
        finished_ready = finished.get_future().share();
    }

    Strand strand;
    redis::config run_config{};
    redis::connection connection;
    std::atomic< bool > running{ false };

    // async_run 완료가 채움. Stop() 을 요청이 아니라 실제 정지로 만드는 것
    std::promise< void > finished{};
    std::shared_future< void > finished_ready{};
};

RedisConnection::RedisConnection( IoContext& io_context, const RedisConnectionConfig& config )
    : impl_( std::make_shared< Impl >( io_context, config ) )
{
}

RedisConnection::~RedisConnection() { Stop(); }

void RedisConnection::Start()
{
    if ( impl_->running.exchange( true ) )
    {
        return;
    }
    // [AD 9.1] 여기가 아니라 strand 위에서 개시
    // async_run 은 connection 내부를 건드린다
    // 이 호출은 프로세스 수명 스레드에서 오므로 mutex 부재의 근거
    std::shared_ptr< Impl > impl = impl_;
    asio::post( impl->strand, Guarded( Ctx{},
                                       [impl]
                                       {
                                           impl->connection.async_run(
                                               impl->run_config,
                                               Guarded( Ctx{},
                                                        [impl]( const ErrorCode& failure )
                                                        {
                                                            // 루프 취소나 영구 포기 때만 도달
                                                            // 평범한 장애는 재접속이 흡수
                                                            ATLAS_LOG_INFO(
                                                                "redis run loop finished: {}",
                                                                failure.message() );
                                                            impl->finished.set_value();
                                                        } ) );
                                       } ) );
}

void RedisConnection::Stop() noexcept
{
    if ( !impl_->running.exchange( false ) )
    {
        return;
    }
    try
    {
        // strand 가 참조를 들고 있어 이 객체가 먼저 사라져도 취소는 안전
        std::shared_ptr< Impl > impl = impl_;
        asio::post( impl->strand, Guarded( Ctx{}, [impl] { impl->connection.cancel(); } ) );
        // 그리고 기다림. IoRunner::Stop() 은 work guard 해제 후 join 뿐
        // 그 순간 남은 실행 루프가 join 을 영영 붙듦
        // 정지는 요청이 아니라 관측 대상
        if ( impl->finished_ready.wait_for( kStopBudget ) != std::future_status::ready )
        {
            ATLAS_LOG_ERROR( "redis run loop did not finish within {}s of being cancelled",
                             kStopBudget.count() );
        }
    }
    catch ( ... )
    {  // NOLINT - noexcept 해체 경로는 보고할 곳이 없음
    }
}

bool RedisConnection::WaitUntilReady( Duration timeout )
{
    const TimePoint deadline = Clock::now() + timeout;
    while ( true )
    {
        auto answered = std::make_shared< std::promise< bool > >();
        std::future< bool > probe = answered->get_future();
        Execute( Ctx{}, RedisCommand{ .verb = "PING", .args = {} },
                 [answered]( const RedisResult& result ) { answered->set_value( result.ok ); } );

        // answered 는 누수가 아님. 두 번째 참조가 handler 안에 있다
        // 분석기가 strand post 지점에서 추적을 멈출 뿐
        // 프로브 만료 후에도 handler 가 남으므로 지역 promise 였다면 dangling
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
        if ( probe.wait_for( kProbeBudget ) == std::future_status::ready && probe.get() )
        {
            return true;
        }
        if ( Clock::now() >= deadline )
        {
            return false;
        }
        std::this_thread::sleep_for( kProbePause );
    }
}

bool RedisConnection::IsConfigured() const noexcept { return !impl_->run_config.addr.host.empty(); }

void RedisConnection::Execute( Ctx ctx, const RedisCommand& command, Handler handler )
{
    if ( !impl_->running.load() )
    {
        // 큐잉하지 않고 즉시 거부. 어느 쪽이든 호출자는 결과를 정확히 1회 받음.
        // 침묵은 read-through 캐시가 회복할 수 없는 유일한 답
        handler( RedisResult{} );
        return;
    }

    auto state = std::make_shared< ExecState >();
    if ( command.args.empty() )
    {
        state->request.push( command.verb );
    }
    else
    {
        state->request.push_range( command.verb, command.args );
    }

    std::shared_ptr< Impl > impl = impl_;
    asio::post(
        impl->strand,
        Guarded( ctx,
                 [impl, state, ctx, handler = std::move( handler )]
                 {
                     impl->connection.async_exec(
                         state->request, state->response,
                         asio::cancel_after(
                             kCommandBudget,
                             Guarded( ctx,
                                      [state, handler]( const ErrorCode& failure, std::size_t )
                                      {
                                          RedisResult result;
                                          result.ok = !failure && state->response.has_value();
                                          if ( result.ok )
                                          {
                                              result.values = FlattenReply( state->response );
                                          }
                                          handler( result );
                                      } ) ) );
                 } ) );
}

}  // namespace atlas
