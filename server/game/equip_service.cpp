// =============================================================================
// 장착 트랜잭션 본체. 서버 권위 검사 -> 슬롯 비우기 -> 차지 -> 커밋
// =============================================================================

#include "game/equip_service.h"

#include <array>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "atlas/db/prepared_statement.h"
#include "atlas/db/transaction.h"
#include "game/character.h"
#include "game/inventory.h"
#include "generated/db/character_items_row.h"
#include "generated/db/characters_row.h"
#include "generated/info/item_info.h"

namespace atlas_demo
{

namespace
{

using atlas::DbRow;
using atlas::DbValue;
using atlas::UInt64;

}  // namespace

std::string_view DescribeEquipResult( EquipResult result ) noexcept
{
    switch ( result )
    {
        case EquipResult::Ok:
            return "ok";
        case EquipResult::InvalidSlot:
            return "invalid slot";
        case EquipResult::ItemNotFound:
            return "no such item";
        case EquipResult::NotOwned:
            return "item belongs to another character";
        case EquipResult::CharacterNotFound:
            return "no such character";
        case EquipResult::UnknownItem:
            return "no such item definition";
        case EquipResult::SlotMismatch:
            return "item does not go in that slot";
    }
    return "unknown";
}

std::mutex& EquipService::LockFor( const CharacterKey& key )
{
    const std::scoped_lock registry_guard( registry_mutex_ );
    std::unique_ptr< std::mutex >& slot = character_locks_[key];
    if ( !slot )
    {
        slot = std::make_unique< std::mutex >();
    }
    return *slot;
}

EquipService::Outcome EquipService::Equip( atlas::Ctx& ctx, atlas::Connection& connection,
                                           const Request& request,
                                           const FaultInjector& before_commit )
{
    if ( !IsEquippableSlot( request.slot ) )
    {
        return { .result = EquipResult::InvalidSlot, .unequipped_item_uid = 0 };
    }

    // 쓰기만이 아니라 읽기-수정-쓰기 전체를 잡음
    // 불변식을 깨는 교차는 "둘 다 슬롯이 비었다고 봤다"
    // 읽기 뒤에 잡는 락은 아무것도 못 지킴
    const std::scoped_lock character_guard(
        LockFor( CharacterKey{ request.server_id, atlas::IdValue( request.character_id ) } ) );

    // [AD 8.2] 서버 권위. (server, uid) 로 찾고 소유자를 비교함
    // WHERE 에 character_id 를 넣으면 "남의 아이템"이 "없는 아이템"으로 접힘
    const std::array< DbValue, 2 > item_lookup{
        DbValue{ static_cast< UInt64 >( request.server_id ) }, DbValue{ request.item_uid } };
    const std::vector< DbRow > item_rows =
        connection.Prepare( kSelectItemByUidSql ).Query( item_lookup );
    if ( item_rows.empty() )
    {
        return { .result = EquipResult::ItemNotFound, .unequipped_item_uid = 0 };
    }

    atlas::generated::CharacterItemsRow target = CharacterItemsRowFromDb( item_rows.front() );
    if ( target.character_id_ != request.character_id )
    {
        ATLAS_LOG_WARN( "equip refused: item {} belongs to character {}, not {}", target.item_uid_,
                        atlas::IdValue( target.character_id_ ),
                        atlas::IdValue( request.character_id ) );
        return { .result = EquipResult::NotOwned, .unequipped_item_uid = 0 };
    }

    // [AD 8.2] row 는 진짜고 이 캐릭터 것
    // 여기서 보는 것은 그 아이템이 무엇인가라는 주장
    // item.csv 가 없던 때는 그 주장을 반박할 근거가 없었음
    // [AD 8.1] 유효한 프레임이라 연결을 닫지 않고 거부
    const atlas::generated::ItemInfoRow* definition =
        atlas::generated::FindItemInfo( target.item_id_ );
    if ( definition == nullptr )
    {
        ATLAS_LOG_WARN( "equip refused: item {} has item_id {}, which item.csv does not define",
                        target.item_uid_, target.item_id_ );
        return { .result = EquipResult::UnknownItem, .unequipped_item_uid = 0 };
    }
    if ( definition->slot_ != static_cast< atlas::UInt8 >( request.slot ) )
    {
        ATLAS_LOG_WARN( "equip refused: item_id {} belongs in slot {}, not {}", target.item_id_,
                        static_cast< atlas::UInt16 >( definition->slot_ ),
                        DescribeSlot( request.slot ) );
        return { .result = EquipResult::SlotMismatch, .unequipped_item_uid = 0 };
    }

    const std::array< DbValue, 2 > character_lookup{
        DbValue{ static_cast< UInt64 >( request.server_id ) },
        DbValue{ atlas::IdValue( request.character_id ) } };
    const std::vector< DbRow > character_rows =
        connection.Prepare( atlas::generated::kCharactersSelectByPkSql ).Query( character_lookup );
    if ( character_rows.empty() )
    {
        return { .result = EquipResult::CharacterNotFound, .unequipped_item_uid = 0 };
    }
    atlas::generated::CharactersRow character = CharactersRowFromDb( character_rows.front() );

    // [AD 10] RAII. 이 줄 이후의 throw 는 스코프를 풀며 롤백함
    // 점유자 읽기가 트랜잭션 안이어야 지우는 대상과 쓰는 대상이 같음
    atlas::Transaction transaction( connection, ctx );

    const std::array< DbValue, 3 > slot_lookup{
        DbValue{ static_cast< UInt64 >( request.server_id ) },
        DbValue{ atlas::IdValue( request.character_id ) },
        DbValue{ static_cast< UInt64 >( static_cast< atlas::UInt8 >( request.slot ) ) } };
    const std::vector< DbRow > occupants =
        connection.Prepare( kSelectItemsInSlotSql ).Query( slot_lookup );

    UInt64 unequipped = 0;
    for ( const DbRow& row : occupants )
    {
        atlas::generated::CharacterItemsRow occupant = CharacterItemsRowFromDb( row );
        if ( occupant.item_uid_ == target.item_uid_ )
        {
            // 이미 착용 중. 같은 값 재기록도 맞지만 영향 행 수를 정직하게 둠
            continue;
        }
        // 첫 번째가 아니라 모든 점유자
        // 제약이 없으니 고쳐야 할 상태를 성립한 것으로 가정하면 안 됨
        // 둘이면 이미 깨진 것이라 시끄럽게 정리
        if ( unequipped != 0 )
        {
            ATLAS_LOG_WARN( "slot {} of character {} held more than one item - clearing all",
                            DescribeSlot( request.slot ), atlas::IdValue( request.character_id ) );
        }
        occupant.equip_slot_ = static_cast< atlas::UInt8 >( EquipSlot::None );
        connection.Prepare( atlas::generated::kCharacterItemsUpdateByPkSql )
            .Execute( CharacterItemsUpdateParameters( occupant ) );
        unequipped = occupant.item_uid_;
    }

    target.equip_slot_ = static_cast< atlas::UInt8 >( request.slot );
    connection.Prepare( atlas::generated::kCharacterItemsUpdateByPkSql )
        .Execute( CharacterItemsUpdateParameters( target ) );

    // 세 번째 쓰기가 같은 트랜잭션에서 characters 를 건드림
    // [AD 3.3] 스키마 예산이 끝나 updated_at 도 파생 스탯 컬럼도 없음
    // last_login_at 을 쓰는 목적은 트랜잭션이 두 테이블에 걸친다는 증명
    character.last_login_at_ = PersistableNow();
    connection.Prepare( atlas::generated::kCharactersUpdateByPkSql )
        .Execute( CharactersUpdateParameters( character ) );

    if ( before_commit )
    {
        before_commit();
    }

    transaction.Commit();

    ATLAS_LOG_INFO( "character {} equipped item {} into slot {} (unequipped {})",
                    atlas::IdValue( request.character_id ), request.item_uid,
                    DescribeSlot( request.slot ), unequipped );
    return { .result = EquipResult::Ok, .unequipped_item_uid = unequipped };
}

}  // namespace atlas_demo
