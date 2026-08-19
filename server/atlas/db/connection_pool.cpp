// =============================================================================
// ConnectionPool 구현. 대여 반납과 대기, 그리고 락 밖에서의 생존 확인
// =============================================================================

#include "atlas/db/connection_pool.h"

#include <memory>
#include <utility>

#include "atlas/core/error.h"
#include "atlas/core/log.h"

namespace atlas
{

PooledConnection::PooledConnection( ConnectionPool& pool, Connection& connection ) noexcept
    : pool_( &pool ), connection_( &connection )
{
}

PooledConnection::PooledConnection( PooledConnection&& other ) noexcept
    : pool_( other.pool_ ), connection_( other.connection_ )
{
    other.pool_ = nullptr;
    other.connection_ = nullptr;
}

PooledConnection& PooledConnection::operator=( PooledConnection&& other ) noexcept
{
    if ( this != &other )
    {
        Return();
        pool_ = other.pool_;
        connection_ = other.connection_;
        other.pool_ = nullptr;
        other.connection_ = nullptr;
    }
    return *this;
}

PooledConnection::~PooledConnection() { Return(); }

void PooledConnection::Return() noexcept
{
    if ( pool_ != nullptr && connection_ != nullptr )
    {
        pool_->Release( *connection_ );
    }
    pool_ = nullptr;
    connection_ = nullptr;
}

ConnectionPool::ConnectionPool( const DbConnectionConfig& config, std::size_t size )
{
    ATLAS_CHECK( size > 0, "connection pool size must be greater than zero" );
    connections_.reserve( size );
    for ( std::size_t index = 0; index < size; ++index )
    {
        connections_.push_back( std::make_unique< Connection >( config ) );
        free_.push_back( connections_.back().get() );
    }
    ATLAS_LOG_INFO( "db connection pool ready: {} connections", size );
}

ConnectionPool::~ConnectionPool()
{
    // 풀보다 오래 사는 대여는 PooledConnection::Return 안의 매달린 포인터
    // 소멸자가 던질 수는 없지만 조용히 있기를 거부할 수는 있음
    const std::lock_guard< std::mutex > lock( mutex_ );
    if ( free_.size() != connections_.size() )
    {
        // core/error.h 의 Guarded::Fail 과 같은 모양. 소멸자는 암묵적 noexcept
        // std::format 이 이 호출 지점에서 돌므로 진단 자체가 던지면 안 됨
        try
        {
            ATLAS_LOG_ERROR( "db connection pool destroyed with {} connection(s) still leased",
                             connections_.size() - free_.size() );
        }
        catch ( ... )  // NOLINT - 더는 보고할 곳이 없음
        {
        }
    }
}

std::size_t ConnectionPool::Available() const
{
    const std::lock_guard< std::mutex > lock( mutex_ );
    return free_.size();
}

std::optional< PooledConnection > ConnectionPool::Acquire( Duration timeout )
{
    std::unique_lock< std::mutex > lock( mutex_ );
    // [CS 4.2] core/time.h 의 단조 Clock 위에서 대기
    // 벽시계면 NTP 보정이 system_clock 을 되돌리는 순간이 문제
    // 유한 대기가 이 함수가 막으려는 무한 대기로 바뀜
    if ( !available_.wait_for( lock, timeout, [this] { return !free_.empty(); } ) )
    {
        ATLAS_LOG_WARN( "db connection pool exhausted: {} connections, none free within timeout",
                        connections_.size() );
        return std::nullopt;
    }

    Connection* connection = free_.front();
    free_.pop_front();
    lock.unlock();

    // unlock 이 여기 있는 이유는 락 밖에서 검사하기 위해서
    // EnsureAlive 는 네트워크 I/O 라 mutex_ 를 쥔 채 하면 전부 소켓 뒤에 줄 섬
    // 이미 free 목록에서 뺐으니 락 없이도 안전
    // 대여 객체를 먼저 만드는 것은 두 출구가 같은 RAII 경로를 쓰게 하려고
    // 죽은 연결도 풀로 돌아가 다음 대여에서 다시 기회를 얻음
    PooledConnection lease( *this, *connection );
    if ( !connection->EnsureAlive() )
    {
        ATLAS_LOG_WARN( "db connection dead on lease and reconnect failed; reporting unavailable" );
        return std::nullopt;
    }
    return { std::move( lease ) };
}

void ConnectionPool::Release( Connection& connection ) noexcept
{
    {
        const std::lock_guard< std::mutex > lock( mutex_ );
        free_.push_back( &connection );
    }
    available_.notify_one();
}

}  // namespace atlas
