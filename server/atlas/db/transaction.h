#pragma once

// =============================================================================
// [AD 10] 커밋 없이 풀리는 스코프는 롤백되는 RAII 트랜잭션과,
// 커밋 이후를 되돌리는 보상 스코프
// =============================================================================

#include <functional>

#include "atlas/core/ctx.h"
#include "atlas/db/connection.h"

namespace atlas
{

// [AD 10] RAII 스코프와 예외 가드는 한 세트
// Guarded 핸들러 안의 throw 가 이 소멸자를 풀고 지나가야만 원자성이 성립
// [AD 9.2] ctx 참조는 thread_local 이 아니라 호출자의 원장
// Guarded 는 진입 시 사본을 심고 이탈 시 이전 값을 되돌림
// 트랜잭션이 열려 있었는지는 가드가 반환한 뒤에 물어야 답이 나옴
class Transaction
{
public:
    // 즉시 트랜잭션을 열고 ctx.tx_state 를 Active 로. 서버가 거부하면 DbException
    Transaction( Connection& connection, Ctx& ctx );

    // Commit() 이 이미 돌지 않았다면 롤백하고 ctx.tx_state 를 RolledBack 으로
    ~Transaction();

    Transaction( const Transaction& ) = delete;
    Transaction& operator=( const Transaction& ) = delete;
    Transaction( Transaction&& ) = delete;
    Transaction& operator=( Transaction&& ) = delete;

    // 커밋하고 ctx.tx_state 를 Committed 로. 실패 시 DbException
    // 이때 소멸자는 더 이상 롤백하지 않음 - 서버가 이미 트랜잭션을 끝냄
    void Commit();

    [[nodiscard]] bool IsOpen() const noexcept { return open_; }

private:
    // [CS 4.4] 비소유 관찰자. 둘 다 구조상 이 스코프보다 오래 삼
    Connection* connection_;
    Ctx* ctx_;
    bool open_{ false };
};

// [AD 10.4] 보상. Transaction 의 거울상이되 시간상 한 걸음 뒤
// 롤백과 보상을 가르는 선은 커밋
// 커밋 전에는 ~Transaction 이 되돌리고 DB 는 인정한 적조차 없음
// 커밋 뒤에는 되돌릴 것이 없고 유일한 길은 역방향 쓰기를 담은 새 커밋
// 그 사이의 창은 모두에게 보임. 보상된 요청은 로그에 남은 두 개의 사실
class PostCommitGuard
{
public:
    // 역방향 쓰기. 커밋을 되돌리는 가장 단순한 한 문장으로 유지할 것
    // 예외가 이미 날아가는 중에 실행될 수 있음
    // 실패 시의 결과는 아래 소멸자
    using Compensation = std::function< void() >;

    explicit PostCommitGuard( Compensation compensation );

    // Release() 가 먼저 돌지 않았다면 보상을 실행
    // 던지는 보상은 잡아서 로그로만 남김
    // 스택 풀림 중 소멸자가 던지면 프로세스가 죽음
    // 그 시점의 커밋 상태는 실제로 불일치이고 그 로그 한 줄이 유일한 기록
    // 보상이 워크플로가 아니라 역방향 쓰기 하나여야 하는 이유
    ~PostCommitGuard();

    PostCommitGuard( const PostCommitGuard& ) = delete;
    PostCommitGuard& operator=( const PostCommitGuard& ) = delete;
    PostCommitGuard( PostCommitGuard&& ) = delete;
    PostCommitGuard& operator=( PostCommitGuard&& ) = delete;

    // 커밋 이후 단계가 성공. 되돌릴 것이 없음
    void Release() noexcept { armed_ = false; }

    [[nodiscard]] bool IsArmed() const noexcept { return armed_; }

private:
    Compensation compensation_;
    bool armed_{ true };
};

}  // namespace atlas
