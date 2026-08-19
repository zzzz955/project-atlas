// =============================================================================
// types.h 별칭의 폭과 부호를 컴파일 타임에 못 박는 회귀 방어선
// =============================================================================

#include "atlas/core/types.h"

#include <type_traits>

namespace atlas
{

// 폭이 dev 와 prod 에서 갈리면 와이어 포맷과 DB 행이 조용히 어긋남
static_assert( sizeof( Int8 ) == 1 && sizeof( UInt8 ) == 1 );
static_assert( sizeof( Int16 ) == 2 && sizeof( UInt16 ) == 2 );
static_assert( sizeof( Int32 ) == 4 && sizeof( UInt32 ) == 4 );
static_assert( sizeof( Int64 ) == 8 && sizeof( UInt64 ) == 8 );
static_assert( sizeof( Float32 ) == 4 );
static_assert( sizeof( Float64 ) == 8 );
static_assert( sizeof( Byte ) == 1 );

static_assert( std::is_signed_v< Int8 > && std::is_signed_v< Int16 > );
static_assert( std::is_signed_v< Int32 > && std::is_signed_v< Int64 > );
static_assert( std::is_unsigned_v< UInt8 > && std::is_unsigned_v< UInt16 > );
static_assert( std::is_unsigned_v< UInt32 > && std::is_unsigned_v< UInt64 > );

}  // namespace atlas
