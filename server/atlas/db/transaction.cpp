// =============================================================================
// Transaction / PostCommitGuard 구현. 두 소멸자 모두 예외 중에 실행될 수 있음
// =============================================================================

#include "atlas/db/transaction.h"

#include <exception>
#include <utility>

#include "atlas/core/error.h"
#include "atlas/core/log.h"

namespace atlas
{

Transaction::Transaction( Connection& connection, Ctx& ctx )
    : connection_( &connection ), ctx_( &ctx )
{
    connection_->Begin();
    open_ = true;
    ctx_->tx_state = TxState::Active;
}

Transaction::~Transaction()
{
    if ( !open_ )
    {
        return;
    }
    // 이 클래스가 존재하는 경우는 예외가 날아가는 중에 도달하는 경우
    // 여기서는 아무것도 던질 수 없음
    // Connection::Rollback 은 noexcept 이고 실패를 스스로 로그
    open_ = false;
    connection_->Rollback();
    ctx_->tx_state = TxState::RolledBack;
}

void Transaction::Commit()
{
    ATLAS_CHECK( open_, "transaction commit on a scope that is not open" );
    // 호출 전에 내림 - Commit 이 던져도 서버는 트랜잭션을 이미 끝낸 상태
    // 그때 소멸자의 롤백은 트랜잭션 밖의 연결에 발행됨
    open_ = false;
    connection_->Commit();
    ctx_->tx_state = TxState::Committed;
}

PostCommitGuard::PostCommitGuard( Compensation compensation )
    : compensation_( std::move( compensation ) )
{
}

PostCommitGuard::~PostCommitGuard()
{
    if ( !armed_ || !compensation_ )
    {
        return;
    }
    armed_ = false;
    try
    {
        compensation_();
    }
    catch ( const std::exception& ex )
    {
        ATLAS_LOG_ERROR( "post-commit compensation failed; committed state is inconsistent: {}",
                         ex.what() );
    }
    catch ( ... )
    {
        ATLAS_LOG_ERROR(
            "post-commit compensation failed with a non-std exception; committed state is "
            "inconsistent" );
    }
}

}  // namespace atlas
