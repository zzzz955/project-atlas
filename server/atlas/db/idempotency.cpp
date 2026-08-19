// =============================================================================
// IdempotencyStore 구현. 조회와 점유가 한 임계 구역인 상태 기계
// =============================================================================

#include "atlas/db/idempotency.h"

#include <utility>

#include "atlas/core/error.h"

namespace atlas
{
namespace
{

// 키가 0 이면 요청 진입점이 채우지 않았다는 뜻
// 라벨 없는 모든 요청이 조용히 같은 요청 하나가 되어버림
UInt64 RequireKey( const Ctx& ctx )
{
    ATLAS_CHECK( ctx.request_key != RequestKey{},
                 "idempotency key is unset; the request entry point must fill ctx.request_key" );
    return RequestKeyValue( ctx.request_key );
}

}  // namespace

IdempotencyStore::IdempotencyStore( Duration entry_ttl ) : entry_ttl_( entry_ttl ) {}

RequestAdmissionResult IdempotencyStore::Begin( Ctx& ctx, Duration wait_budget )
{
    const UInt64 key = RequireKey( ctx );
    const TimePoint wait_deadline = Clock::now() + wait_budget;

    std::unique_lock< std::mutex > lock( mutex_ );
    bool waited_out = false;
    for ( ;; )
    {
        const auto found = entries_.find( key );
        const bool live = found != entries_.end() && Clock::now() < found->second.expires_at;

        if ( !live )
        {
            // 조회와 점유는 한 임계 구역
            // 검사 - 해제 - 삽입으로 쪼개면 동시 도착 둘이 모두 주인이 됨
            // 그 이중 실행은 숫자가 안 맞기 시작할 때까지 보이지 않음
            Entry fresh;
            fresh.state = RequestState::Received;
            fresh.expires_at = Clock::now() + entry_ttl_;
            entries_.insert_or_assign( key, std::move( fresh ) );
            ctx.request_state = RequestState::Received;
            return { .admission = RequestAdmission::Started,
                     .state = RequestState::Received,
                     .result = std::string{} };
        }

        if ( found->second.state != RequestState::Received )
        {
            ctx.request_state = found->second.state;
            return { .admission = RequestAdmission::Replay,
                     .state = found->second.state,
                     .result = found->second.result };
        }

        // 남이 쥐었고 아직 실행 중
        // 예산 만료 후 한 번 더 확인. 대기 도중 도착한 완료로 답하게 함
        if ( waited_out )
        {
            ctx.request_state = RequestState::Received;
            return { .admission = RequestAdmission::InProgress,
                     .state = RequestState::Received,
                     .result = std::string{} };
        }
        waited_out = finished_.wait_until( lock, wait_deadline ) == std::cv_status::timeout;
    }
}

void IdempotencyStore::MarkPersisted( Ctx& ctx, std::string result )
{
    // 커밋 후 응답 순서를 기계로 만든 것
    // 여기서 열린 트랜잭션은 DB 가 아직 받지 않은 것을 말하기 한 걸음 전
    ATLAS_CHECK( ctx.tx_state != TxState::Active,
                 "request marked Persisted while its transaction is still open - the response must "
                 "never precede the commit" );
    const UInt64 key = RequireKey( ctx );

    {
        std::lock_guard< std::mutex > lock( mutex_ );
        const auto found = entries_.find( key );
        ATLAS_CHECK( found != entries_.end(),
                     "request marked Persisted without an admission record; the key expired or "
                     "Begin was never called" );
        found->second.state = RequestState::Persisted;
        found->second.result = std::move( result );
        found->second.expires_at = Clock::now() + entry_ttl_;
    }
    ctx.request_state = RequestState::Persisted;
    finished_.notify_all();
}

void IdempotencyStore::MarkResponded( Ctx& ctx )
{
    const UInt64 key = RequireKey( ctx );

    {
        std::lock_guard< std::mutex > lock( mutex_ );
        const auto found = entries_.find( key );
        ATLAS_CHECK( found != entries_.end(),
                     "request marked Responded without an admission record" );
        ATLAS_CHECK( found->second.state == RequestState::Persisted,
                     "request marked Responded without passing through Persisted" );
        found->second.state = RequestState::Responded;
    }
    ctx.request_state = RequestState::Responded;
}

void IdempotencyStore::Seed( Ctx& ctx, std::string result )
{
    const UInt64 key = RequireKey( ctx );

    {
        std::lock_guard< std::mutex > lock( mutex_ );
        Entry recovered;
        recovered.state = RequestState::Persisted;
        recovered.result = std::move( result );
        recovered.expires_at = Clock::now() + entry_ttl_;
        entries_.insert_or_assign( key, std::move( recovered ) );
    }
    ctx.request_state = RequestState::Persisted;
    finished_.notify_all();
}

void IdempotencyStore::Abandon( Ctx& ctx )
{
    const UInt64 key = RequireKey( ctx );

    {
        std::lock_guard< std::mutex > lock( mutex_ );
        entries_.erase( key );
    }
    ctx.request_state = RequestState::None;
    // 완료된 것이 없어도 대기자는 깨워야 함. 키가 비었고 그중 하나가 새 주인
    // 예산이 다 될 때까지 재우면 없는 요청을 "진행 중" 이라 보고하게 됨
    finished_.notify_all();
}

std::optional< RequestState > IdempotencyStore::Find( RequestKey key ) const
{
    std::lock_guard< std::mutex > lock( mutex_ );
    const auto found = entries_.find( RequestKeyValue( key ) );
    if ( found == entries_.end() || Clock::now() >= found->second.expires_at )
    {
        return std::nullopt;
    }
    return found->second.state;
}

std::size_t IdempotencyStore::PurgeExpired()
{
    const TimePoint now = Clock::now();
    std::lock_guard< std::mutex > lock( mutex_ );
    const std::size_t before = entries_.size();
    for ( auto it = entries_.begin(); it != entries_.end(); )
    {
        if ( now >= it->second.expires_at )
        {
            it = entries_.erase( it );
        }
        else
        {
            ++it;
        }
    }
    return before - entries_.size();
}

std::size_t IdempotencyStore::Size() const
{
    std::lock_guard< std::mutex > lock( mutex_ );
    return entries_.size();
}

}  // namespace atlas
