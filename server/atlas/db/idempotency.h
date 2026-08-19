#pragma once

// =============================================================================
// [AD 10.4] 경계를 넘어온 요청의 at-most-once 처리
// 발신자가 만든 멱등 키 기준으로 한 번만 실행하고 반복에는 기록을 돌려줌
// =============================================================================

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "atlas/core/ctx.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"

namespace atlas
{

// Begin() 이 이 호출자에게 내린 결정
enum class RequestAdmission : UInt8
{
    // 이 호출자가 키를 소유. 요청을 실행할 것
    Started,
    // 키가 이미 완료됨. 기록된 결과를 쓸 것. 실행하지 말 것
    Replay,
    // 다른 호출자가 키를 쥐었고 대기 예산 안에 끝내지 못함. 실행된 것도 결과도 없음
    InProgress,
};

struct RequestAdmissionResult
{
    RequestAdmission admission{ RequestAdmission::Started };
    RequestState state{ RequestState::None };
    // Replay 일 때만 비지 않음. 실제 실행이 기록한 결과 블롭
    // 이 계층에는 불투명 - 해석하지 않고 같은 바이트를 돌려준다는 보장만 함
    std::string result{};
};

// "재전송" 과 "진짜 새 중복 요청" 은 수신 측에서 구분 불가
// 발신자가 만든 멱등 키만이 그 사실을 피하지 않고 마주하는 구성
// [AD 3.3] 메모리에만 있음. 데모 스키마의 테이블 예산이 다 참
// 재시작을 넘기는 것은 커밋된 DB 상태뿐이라 복구는 Seed() 로 함
// [AD 9.1] 상태 전이가 원자적이지 않으면 같은 키의 동시 도착 둘이 다 실행됨
class IdempotencyStore
{
public:
    // entry_ttl 은 요청이 걸릴 수 있는 최장 시간보다 커야 함
    // 실행 타임아웃이 아니라 완료 기록의 메모리 상한
    // 실행 중인 요청의 기록이 만료되면 재전송이 두 번째 실행을 시작함
    explicit IdempotencyStore( Duration entry_ttl );

    // ctx.request_key 를 실행용으로 점유하거나 이미 알려진 것을 보고
    // 다른 곳에서 진행 중이면 wait_budget 만큼 Persisted 도달을 기다림
    // 실행 도중 도착한 재전송은 두 번 실행되지 않음
    // 대기가 유한한 이유는 ConnectionPool::Acquire 와 같음
    // 결과 상태는 ctx.request_state 에 기록
    RequestAdmissionResult Begin( Ctx& ctx, Duration wait_budget );

    // 커밋된 결과를 기록하고 이 키의 모든 대기자를 깨움
    // 응답은 커밋 뒤에 나가고 절대 그 전이 아님
    // 먼저 답하면 DB 가 끝내 받지 않을 결과를 클라이언트가 쥠
    // 구현의 Active 검사가 그 순서를 기억이 아니라 기계로 만든 것
    void MarkPersisted( Ctx& ctx, std::string result );

    // 클라이언트에게 알렸음을 기록. Persisted 를 거치지 않은 키는 거부
    // 상태 기계는 이어받을 수는 있어도 건너뛸 수는 없음
    void MarkResponded( Ctx& ctx );

    // 재시작 후 지속 상태에서 Persisted 기록을 재구성해 심음
    // 이 프로세스가 실행한 적 없는 요청이 대상
    // 이어받는 것이지 재생이 아님. 정확한 응답 바이트는 프로세스와 함께 사라짐
    // 재구성은 지속 상태가 결정해 주는 만큼만 충실
    void Seed( Ctx& ctx, std::string result );

    // 키를 잊어 재전송이 처음부터 실행되게 함
    // 요청이 결국 일어나지 않은 두 경우용 - 트랜잭션 롤백, 또는 커밋 이후 보상
    // 보상은 transaction.h 의 PostCommitGuard
    // 대기자는 풀려 다시 판단하고 그중 하나가 새 주인
    void Abandon( Ctx& ctx );

    [[nodiscard]] std::optional< RequestState > Find( RequestKey key ) const;

    // 만료 기록을 버리고 버린 수를 반환
    // 아직 타이머로 부르는 곳이 없음 - 타이머 주인은 아직 없는 요청 계층
    // 부를 수 있는데 안 부르는 것은 눈에 보임
    // 아무도 요청하지 않은 백그라운드 스레드는 보이지 않음
    std::size_t PurgeExpired();

    [[nodiscard]] std::size_t Size() const;

private:
    struct Entry
    {
        RequestState state{ RequestState::Received };
        std::string result{};
        TimePoint expires_at{};
    };

    mutable std::mutex mutex_;
    std::condition_variable finished_;
    // 원시 값으로 키잉
    // RequestKey 는 스코프 열거형이라 해싱하려면 이 맵만 쓰는 특수화가 필요
    std::unordered_map< UInt64, Entry > entries_;
    Duration entry_ttl_;
};

}  // namespace atlas
