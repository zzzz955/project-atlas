// =============================================================================
// CRC-32 룩업 테이블과 누적 함수. 테이블은 컴파일 타임에 생성
// =============================================================================

#include "atlas/proto/crc32.h"

#include <array>
#include <cstddef>

namespace atlas
{
namespace
{

// [CS 2.2] 익명 네임스페이스인데도 Crc32 접두사
// 유니티 빌드가 최대 8개 소스를 한 TU 로 합쳐 파일 스코프 격리가 사라진다
// 밋밋한 이름은 충돌한다
constexpr std::size_t kCrc32TableSize = 256;

constexpr std::array< UInt32, kCrc32TableSize > MakeCrc32Table()
{
    std::array< UInt32, kCrc32TableSize > table{};
    for ( UInt32 index = 0; index < static_cast< UInt32 >( kCrc32TableSize ); ++index )
    {
        UInt32 remainder = index;
        for ( UInt32 bit = 0; bit < 8U; ++bit )
        {
            remainder = ( ( remainder & 1U ) != 0U ) ? ( 0xEDB88320U ^ ( remainder >> 1U ) )
                                                     : ( remainder >> 1U );
        }
        table[index] = remainder;
    }
    return table;
}

// 컴파일 타임 생성이라 초기화 순서 문제도 런타임 준비 비용도 없음
constexpr std::array< UInt32, kCrc32TableSize > kCrc32Table = MakeCrc32Table();

}  // namespace

UInt32 Crc32Update( UInt32 state, std::span< const Byte > data ) noexcept
{
    UInt32 remainder = state;
    for ( const Byte value : data )
    {
        const UInt32 index = ( remainder ^ std::to_integer< UInt32 >( value ) ) & 0xFFU;
        remainder = kCrc32Table[static_cast< std::size_t >( index )] ^ ( remainder >> 8U );
    }
    return remainder;
}

UInt32 Crc32Finish( UInt32 state ) noexcept { return state ^ 0xFFFFFFFFU; }

}  // namespace atlas
