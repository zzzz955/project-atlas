#pragma once

// =============================================================================
// [AD 9.2] 요청 진입점에서 만들어 모든 스레드 경계를 값으로 건너는 원장
// 로그 한 줄과 예외 메시지에 필요한 것만 담음
// =============================================================================

#include <type_traits>

#include "atlas/core/ids.h"
#include "atlas/core/types.h"

namespace atlas
{

// [AD 10] 트랜잭션 범위는 RAII
// 원장이 상태를 들어야 Guarded 실패를 구분할 수 있다
// "열린 것이 없음" 과 "열려 있어 롤백 필요" 는 다르다
enum class TxState : UInt8
{
    None,
    Active,
    Committed,
    RolledBack
};

// [AD 10.4] 송신자가 만든 요청 멱등 키
// 생성과 비교만 하는 64비트 값이 둘이라 강타입으로 나눈다
// [CS 4.3] core/ids.h 는 엔티티 신원 전용이라 여기 두지 않는다
enum class RequestKey : UInt64
{
};

constexpr UInt64 RequestKeyValue( RequestKey key ) noexcept { return static_cast< UInt64 >( key ); }

// [AD 10.4] None -> Received -> Persisted -> Responded
// 순서가 곧 계약. 재개는 되지만 건너뛰기는 안 됨
// 응답이 커밋을 앞지를 수 없는 근거
enum class RequestState : UInt8
{
    None,
    Received,
    Persisted,
    Responded
};

// trace_id 만 감싸지 않은 64비트
// 혼동할 만한 request_key 가 강타입이라 뒤바뀔 짝이 없다
struct Ctx
{
    UInt64 trace_id{ 0 };
    // {0} 은 MSVC 가 scoped enum 기본 멤버 초기화에서 거부. {} 가 미설정을 뜻함
    AccountId account_id{};
    CharacterId character_id{};
    SessionId session_id{};
    RequestKey request_key{};
    TxState tx_state{ TxState::None };
    RequestState request_state{ RequestState::None };
};

// [AD 9.2] 값 복사로 스레드 경계를 넘으므로 필드 추가는 복사 비용 문제
// 둘 중 하나를 깨는 필드는 이 두 assert 를 먼저 설득할 것
static_assert( std::is_trivially_copyable_v< Ctx >,
               "Ctx is copied across every thread boundary; keep the copy a memcpy" );
static_assert( sizeof( Ctx ) <= 64,
               "Ctx is copied on every thread hop; keep it within a cache line" );

// 지금 이 스레드에 설치된 원장. null 없음. 미설정은 기본 생성 Ctx
const Ctx& CurrentCtx() noexcept;

// [AD 9.2] strand 는 핸들러를 직렬화할 뿐 스레드에 고정하지 않는다
// 실행 스레드는 매번 바뀐다
// 진입 설치 - 이탈 복원의 이 RAII 가 정확성을 만든다
class CtxScope
{
public:
    explicit CtxScope( const Ctx& ctx ) noexcept;
    ~CtxScope();

    CtxScope( const CtxScope& ) = delete;
    CtxScope& operator=( const CtxScope& ) = delete;
    CtxScope( CtxScope&& ) = delete;
    CtxScope& operator=( CtxScope&& ) = delete;

private:
    Ctx previous_;
};

}  // namespace atlas
