// =============================================================================
// 프레임 인코드/헤더 디코드와 체크섬 계산
// =============================================================================

#include "atlas/proto/frame.h"

#include <array>

#include "atlas/proto/crc32.h"

namespace atlas
{
namespace
{

// [CS 2.2] 익명 네임스페이스라도 Frame 접두사. 유니티 빌드에서 충돌 방지
// [AD 15.4] core-purity 가 generated 헤더 포함을 막아 중복 구현이 의도적

void FramePutLe16( std::vector< Byte >& out, UInt16 value )
{
    out.push_back( static_cast< Byte >( value & 0xFFU ) );
    out.push_back( static_cast< Byte >( ( value >> 8U ) & 0xFFU ) );
}

void FramePutLe32( std::vector< Byte >& out, UInt32 value )
{
    out.push_back( static_cast< Byte >( value & 0xFFU ) );
    out.push_back( static_cast< Byte >( ( value >> 8U ) & 0xFFU ) );
    out.push_back( static_cast< Byte >( ( value >> 16U ) & 0xFFU ) );
    out.push_back( static_cast< Byte >( ( value >> 24U ) & 0xFFU ) );
}

// 경계 검사 없음. 바이트가 다 있음을 호출자가 먼저 증명한 뒤에만 불림
UInt16 FrameGetLe16( std::span< const Byte > in ) noexcept
{
    return static_cast< UInt16 >(
        std::to_integer< UInt16 >( in[0] ) |
        static_cast< UInt16 >( std::to_integer< UInt16 >( in[1] ) << 8U ) );
}

UInt32 FrameGetLe32( std::span< const Byte > in ) noexcept
{
    return std::to_integer< UInt32 >( in[0] ) | ( std::to_integer< UInt32 >( in[1] ) << 8U ) |
           ( std::to_integer< UInt32 >( in[2] ) << 16U ) |
           ( std::to_integer< UInt32 >( in[3] ) << 24U );
}

}  // namespace

UInt32 FrameChecksum( UInt16 opcode, UInt32 seq, std::span< const Byte > payload ) noexcept
{
    // 검사 구간은 헤더 2..8바이트(opcode, seq) 뒤에 페이로드를 이은 가상 연결
    // 스택 6바이트로 앞부분만 재현하면 페이로드를 복사하지 않아도 됨
    const std::array< Byte, 6 > prefix{
        static_cast< Byte >( opcode & 0xFFU ),
        static_cast< Byte >( ( opcode >> 8U ) & 0xFFU ),
        static_cast< Byte >( seq & 0xFFU ),
        static_cast< Byte >( ( seq >> 8U ) & 0xFFU ),
        static_cast< Byte >( ( seq >> 16U ) & 0xFFU ),
        static_cast< Byte >( ( seq >> 24U ) & 0xFFU ),
    };

    UInt32 state = Crc32Update( kCrc32Init, std::span< const Byte >( prefix ) );
    state = Crc32Update( state, payload );
    return Crc32Finish( state );
}

bool EncodeFrame( std::vector< Byte >& out, UInt16 opcode, UInt32 seq,
                  std::span< const Byte > payload )
{
    // 덧붙이기 전에 검사. 반쯤 쓰인 프레임이 남으면 다음 Send 가 이어 붙음
    if ( payload.size() > kMaxPayload )
    {
        return false;
    }

    // kMaxPayload(16 KiB)는 UInt16 상한보다 한참 아래라 축소 캐스트가 안전
    const auto length = static_cast< UInt16 >( payload.size() );

    out.reserve( out.size() + kFrameHeaderSize + payload.size() );
    FramePutLe16( out, length );
    FramePutLe16( out, opcode );
    FramePutLe32( out, seq );
    FramePutLe32( out, FrameChecksum( opcode, seq, payload ) );
    out.insert( out.end(), payload.begin(), payload.end() );
    return true;
}

bool DecodeFrameHeader( std::span< const Byte > in, FrameHeader& out ) noexcept
{
    if ( in.size() < kFrameHeaderSize )
    {
        return false;
    }

    out.length = FrameGetLe16( in.subspan( 0, 2 ) );
    out.opcode = FrameGetLe16( in.subspan( 2, 2 ) );
    out.seq = FrameGetLe32( in.subspan( 4, 4 ) );
    out.crc32 = FrameGetLe32( in.subspan( 8, 4 ) );
    return true;
}

}  // namespace atlas
