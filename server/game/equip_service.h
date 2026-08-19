#pragma once

// =============================================================================
// 아이템 장착 1건을 원자적으로 처리
// 슬롯당 1개 불변식을 DB 제약 없이 트랜잭션과 캐릭터별 락으로 지킴
// =============================================================================

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "game/inventory.h"

namespace atlas_demo
{

// 장착 거부 사유
// [AD 11.2a] 예상된 실패라 예외가 아니라 반환값
// 여기서 throw 하는 것은 DB 가 문을 거부하는 경우
// 그때 Transaction 이 풀리며 전부 롤백됨
enum class EquipResult : atlas::UInt8
{
    Ok = 0,
    InvalidSlot,
    ItemNotFound,
    NotOwned,  // [AD 8.2] 서버 권위. 아이템은 있지만 이 캐릭터 것이 아님
    CharacterNotFound,
    // [AD 8.2] 이번엔 정적 데이터 대상. row 는 진짜고 내 것
    // 그래도 "그 아이템이 무엇인가"라는 주장은 서버가 거부
    // [AD 8.5] 와이어의 응답 바이트라 중간 삽입이 아니라 끝에 추가
    UnknownItem,   // item.csv 에 정의가 없는 item_id
    SlotMismatch,  // 아이템은 있지만 요청한 슬롯의 것이 아님
};

[[nodiscard]] std::string_view DescribeEquipResult( EquipResult result ) noexcept;

// =============================================================================
// 원자적 장착
// =============================================================================

// 슬롯당 1개는 MySQL 에 선언 불가. 부분 유니크 인덱스가 없음
// 방어선은 아래 트랜잭션과 그것을 깨보는 테스트 둘뿐
// 단일 행 UPDATE 는 트랜잭션 없이도 원자적임
// 두 테이블 세 번의 쓰기여야 롤백이 관측됨

// [AD 9] 캐릭터별 직렬화에 strand 가 아니라 락
// DB 클라이언트가 동기라 DB 스레드 풀에서 돎
// strand 에 고정하면 블로킹 질의가 I/O 스레드를 멈춤
// [AD 9.1] 락은 한 호출 안에서 잡고 놓아 async 경계를 넘지 않음
class EquipService
{
public:
    struct Request
    {
        atlas::UInt16 server_id{ 0 };
        // [AD 8.2] 패킷 값이 아니라 SESSION 의 캐릭터
        // 로그인 이후 모든 것은 연결에서 신원을 파생
        // 인증이 오기 전에 존재하는 서버 권위의 절반
        atlas::CharacterId character_id{};
        atlas::UInt64 item_uid{ 0 };
        EquipSlot slot{ EquipSlot::None };
    };

    struct Outcome
    {
        EquipResult result{ EquipResult::Ok };
        atlas::UInt64 unequipped_item_uid{ 0 };  // 슬롯에서 나간 아이템, 없으면 0
    };

    // 결함 주입 이음매를 프로덕션 시그니처에 의도적으로 둠
    // 그 쓰기를 원할 때 실패시킬 수 없으면 불변식 보존을 밖에서 관측 불가
    // 프로덕션에서는 비어 있고 넘기는 것은 테스트뿐
    using FaultInjector = std::function< void() >;

    EquipService() = default;

    EquipService( const EquipService& ) = delete;
    EquipService& operator=( const EquipService& ) = delete;
    EquipService( EquipService&& ) = delete;
    EquipService& operator=( EquipService&& ) = delete;

    // DB 스레드에서 임대 커넥션으로 실행
    // [AD 10.3] ctx 는 이 작업의 장부
    // 트랜잭션 범위가 tx_state 를 여기 씀
    // 가드가 끝난 뒤에도 호출자가 "열려 있었나"를 물을 수 있음
    Outcome Equip( atlas::Ctx& ctx, atlas::Connection& connection, const Request& request,
                   const FaultInjector& before_commit = {} );

private:
    using CharacterKey = std::pair< atlas::UInt16, atlas::UInt64 >;

    std::mutex& LockFor( const CharacterKey& key );

    // 항목을 지우지 않음. 지우려면 아무도 곧 잡지 않는다는 증명이 필요함
    // 그것이 락 레지스트리가 use-after-free 를 얻는 전형적 경로
    // [AD 10.2] 메모리가 문제인 규모의 답은 작은 맵이 아니라 분산 락
    std::mutex registry_mutex_;
    std::map< CharacterKey, std::unique_ptr< std::mutex > > character_locks_;
};

}  // namespace atlas_demo
