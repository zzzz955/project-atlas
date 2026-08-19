#pragma once

// =============================================================================
// 고정 크기 드라이버 연결 풀과 그 RAII 대여 핸들
// 풀 자체가 이 계층에서 락이 옳은 유일한 자리
// =============================================================================

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "atlas/core/time.h"
#include "atlas/db/connection.h"

namespace atlas
{

class ConnectionPool;

// RAII 대여. 이 객체가 죽으면 연결이 풀로 돌아감
// 예외가 보유 프레임을 훑고 지나가는 경로에서도 마찬가지
class PooledConnection
{
public:
    ~PooledConnection();

    PooledConnection( const PooledConnection& ) = delete;
    PooledConnection& operator=( const PooledConnection& ) = delete;
    // Acquire 가 optional 에 담아 반환하려면 이동 가능해야 함
    // 이동된 대여는 아무것도 소유하지 않고 아무것도 반환하지 않음
    PooledConnection( PooledConnection&& other ) noexcept;
    PooledConnection& operator=( PooledConnection&& other ) noexcept;

    [[nodiscard]] Connection& operator*() const noexcept { return *connection_; }
    [[nodiscard]] Connection* operator->() const noexcept { return connection_; }

private:
    friend class ConnectionPool;
    PooledConnection( ConnectionPool& pool, Connection& connection ) noexcept;

    void Return() noexcept;

    // [CS 4.4] 둘 다 비소유 관찰자
    // 풀이 연결을 소유하고 자신이 내준 모든 대여보다 오래 삼
    ConnectionPool* pool_{ nullptr };
    Connection* connection_{ nullptr };
};

// 고정 크기. 리사이즈 - 오토스케일 - 헬스체크 - 서킷 브레이커 전부 없음
// Phase-1 형태는 고정 풀 + 고갈 시 명확한 실패
// 잘라낸 것 중 돌아온 것은 대여 시 최대 1회 재접속뿐(Connection::EnsureAlive)
// [AD 9.1] 이 계층에서 락이 옳은 유일한 자리. 풀은 진짜 공유 자원
// 세마포어면 free 리스트용 뮤텍스가 또 필요해 "허가 후 pop" 이 원자적이지 않음
class ConnectionPool
{
public:
    // size 개를 모두 즉시 개통. 하나라도 실패하면 DbException
    // 잘못된 자격 증명이 첫 요청 때의 놀람이 아니라 기동 실패가 되게 함
    ConnectionPool( const DbConnectionConfig& config, std::size_t size );
    ~ConnectionPool();

    ConnectionPool( const ConnectionPool& ) = delete;
    ConnectionPool& operator=( const ConnectionPool& ) = delete;
    ConnectionPool( ConnectionPool&& ) = delete;
    ConnectionPool& operator=( ConnectionPool&& ) = delete;

    [[nodiscard]] std::size_t Size() const noexcept { return connections_.size(); }
    [[nodiscard]] std::size_t Available() const;

    // 무한 대기 안 함. 고갈된 풀에서 무한히 기다리면 장애가 멈춤처럼 보임
    // DB 스레드 수가 고정이라 대기자 하나가 곧 일을 그만둔 스레드 하나
    // [AD 11.2a] 예상된 실패라 throw 대신 nullopt
    // 타임아웃 안에 빈 연결이 없었거나, 있던 연결이 죽고 재개통도 실패한 경우
    // 호출자 입장에서 답은 어느 쪽이든 같음
    [[nodiscard]] std::optional< PooledConnection > Acquire( Duration timeout );

private:
    friend class PooledConnection;
    void Release( Connection& connection ) noexcept;

    std::vector< std::unique_ptr< Connection > > connections_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque< Connection* > free_;
};

}  // namespace atlas
