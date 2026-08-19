// =============================================================================
// characters row 와 Character 도메인 사이의 매핑
// =============================================================================

#include "game/character.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "atlas/core/error.h"
#include "atlas/core/ids.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/db/connection.h"
#include "generated/db/characters_row.h"

namespace atlas_demo
{

namespace
{

using atlas::DbValue;
using atlas::Int64;
using atlas::UInt64;

// [CS 2.2] 헬퍼 이름에 테이블명을 실음
// unity build 는 여러 소스를 한 TU 로 합침
// 같은 이름의 익명 네임스페이스 헬퍼끼리 충돌함

UInt64 ReadCharacterUnsigned( const atlas::DbRow& row, std::size_t column )
{
    ATLAS_CHECK( column < row.size(), "characters row is missing column {}", column );
    const UInt64* value = std::get_if< UInt64 >( &row[column] );
    ATLAS_CHECK( value != nullptr, "characters column {} is not an unsigned value", column );
    return *value;
}

Int64 ReadCharacterSigned( const atlas::DbRow& row, std::size_t column )
{
    ATLAS_CHECK( column < row.size(), "characters row is missing column {}", column );
    const Int64* value = std::get_if< Int64 >( &row[column] );
    ATLAS_CHECK( value != nullptr, "characters column {} is not a signed value", column );
    return *value;
}

std::string ReadCharacterText( const atlas::DbRow& row, std::size_t column )
{
    ATLAS_CHECK( column < row.size(), "characters row is missing column {}", column );
    const std::string* value = std::get_if< std::string >( &row[column] );
    ATLAS_CHECK( value != nullptr, "characters column {} is not text", column );
    return *value;
}

atlas::SysTime ReadCharacterTime( const atlas::DbRow& row, std::size_t column )
{
    ATLAS_CHECK( column < row.size(), "characters row is missing column {}", column );
    const atlas::SysTime* value = std::get_if< atlas::SysTime >( &row[column] );
    ATLAS_CHECK( value != nullptr, "characters column {} is not a timestamp", column );
    return *value;
}

}  // namespace

atlas::SysTime PersistableNow()
{
    return std::chrono::floor< std::chrono::seconds >( atlas::SysClock::now() );
}

Character::Character( atlas::UInt16 server_id, atlas::CharacterId character_id,
                      atlas::AccountId account_uid, std::string name, atlas::Int32 pos_x,
                      atlas::Int32 pos_y, atlas::UInt16 level, atlas::UInt64 exp )
    // 월드 신원을 별도 카운터로 발급하지 않고 영속 id 에서 파생
    // 카운터가 둘이면 같은 캐릭터가 묻는 서버에 따라 다른 id 로 답함
    : Entity( static_cast< atlas::ActorId >( atlas::IdValue( character_id ) ), pos_x, pos_y,
              level ),
      server_id_( server_id ),
      character_id_( character_id ),
      account_uid_( account_uid ),
      name_( std::move( name ) ),
      exp_( exp )
{
}

Character CharacterFromRow( const atlas::generated::CharactersRow& row )
{
    return { row.server_id_, row.character_id_, row.account_uid_, row.name_,
             row.pos_x_,     row.pos_y_,        row.level_,       row.exp_ };
}

atlas::generated::CharactersRow CharactersRowFromDb( const atlas::DbRow& row )
{
    atlas::generated::CharactersRow mapped;
    mapped.server_id_ = static_cast< atlas::UInt16 >(
        ReadCharacterUnsigned( row, atlas::generated::kCharactersColServerId ) );
    mapped.character_id_ = static_cast< atlas::CharacterId >(
        ReadCharacterUnsigned( row, atlas::generated::kCharactersColCharacterId ) );
    mapped.account_uid_ = static_cast< atlas::AccountId >(
        ReadCharacterUnsigned( row, atlas::generated::kCharactersColAccountUid ) );
    mapped.name_ = ReadCharacterText( row, atlas::generated::kCharactersColName );
    mapped.pos_x_ = static_cast< atlas::Int32 >(
        ReadCharacterSigned( row, atlas::generated::kCharactersColPosX ) );
    mapped.pos_y_ = static_cast< atlas::Int32 >(
        ReadCharacterSigned( row, atlas::generated::kCharactersColPosY ) );
    mapped.level_ = static_cast< atlas::UInt16 >(
        ReadCharacterUnsigned( row, atlas::generated::kCharactersColLevel ) );
    mapped.exp_ = ReadCharacterUnsigned( row, atlas::generated::kCharactersColExp );
    mapped.created_at_ = ReadCharacterTime( row, atlas::generated::kCharactersColCreatedAt );

    // [AD 10.3] 유일한 nullable 컬럼
    // SQL NULL 은 monostate 로 도착함
    // 여기가 0 이 아니라 빈 optional 로 바뀌는 자리
    ATLAS_CHECK( atlas::generated::kCharactersColLastLoginAt < row.size(),
                 "characters row is missing the last_login_at column" );
    const atlas::DbValue& last_login = row[atlas::generated::kCharactersColLastLoginAt];
    if ( std::holds_alternative< std::monostate >( last_login ) )
    {
        mapped.last_login_at_.reset();
    }
    else
    {
        mapped.last_login_at_ =
            ReadCharacterTime( row, atlas::generated::kCharactersColLastLoginAt );
    }
    return mapped;
}

std::vector< atlas::DbValue > CharactersUpdateParameters(
    const atlas::generated::CharactersRow& row )
{
    const std::array< DbValue, atlas::generated::kCharactersColumnCount > by_column{
        DbValue{ static_cast< UInt64 >( row.server_id_ ) },
        DbValue{ atlas::IdValue( row.character_id_ ) },
        DbValue{ atlas::IdValue( row.account_uid_ ) },
        DbValue{ row.name_ },
        DbValue{ static_cast< Int64 >( row.pos_x_ ) },
        DbValue{ static_cast< Int64 >( row.pos_y_ ) },
        DbValue{ static_cast< UInt64 >( row.level_ ) },
        DbValue{ row.exp_ },
        DbValue{ row.created_at_ },
        row.last_login_at_.has_value() ? DbValue{ *row.last_login_at_ }
                                       : DbValue{ std::monostate{} } };

    std::vector< DbValue > parameters;
    parameters.reserve( atlas::generated::kCharactersUpdateByPkBinding.size() );
    for ( const std::size_t column : atlas::generated::kCharactersUpdateByPkBinding )
    {
        parameters.push_back( by_column[column] );
    }
    return parameters;
}

}  // namespace atlas_demo
