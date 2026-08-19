#pragma once

// =============================================================================
// [AD 8.1] 프레임 계층. Session 은 바이트 경계만 지킨다
// 프레임은 그 위 계층으로 분리되고 의존 방향은 proto -> net 한쪽뿐
// =============================================================================

#include <cstddef>
#include <span>
#include <vector>

#include "atlas/core/types.h"

namespace atlas
{

// 헤더 12바이트 = length UInt16 + opcode UInt16 + seq UInt32 + crc32 UInt32
// 전부 리틀엔디언 고정. length 는 페이로드 바이트 수로 헤더를 세지 않는다
// [CS 4.4] pack 프래그마 없음. 패딩이 컴파일러마다 달라 필드별로 읽고 쓴다
inline constexpr std::size_t kFrameHeaderSize = 12;

// [AD 9] 역압 상한의 절반. 나머지 절반은 Session::kMaxWriteQueueBytes
// 어느 쪽이든 넘기면 연결 종료
inline constexpr std::size_t kMaxPayload = std::size_t{ 16 } * 1024;

// [AD 8.2] HMAC 필드도 그 자리표시자도 없다
// 계층 2는 세션 키를 요구하고 그것은 platform-auth 핸드셰이크를 끌고 온다
// 0으로 채운 예약 필드는 보호처럼 보이기만 한다
// 클라이언트가 없어 나중에 12->20바이트로 넓히는 비용은 0
struct FrameHeader
{
    UInt16 length{ 0 };
    UInt16 opcode{ 0 };
    UInt32 seq{ 0 };
    UInt32 crc32{ 0 };
};

// 완성된 메시지 1개. opcode 의 의미는 이 계층이 모르고 UInt16 을 나를 뿐
// [AD 14] opcode -> 핸들러 표는 게임 쪽 자산
struct Frame
{
    UInt16 opcode{ 0 };
    UInt32 seq{ 0 };
    std::vector< Byte > payload;
};

// [AD 8.1] 검사 구간은 opcode, seq, payload 를 이은 가상 연결
// 헤더 두 필드를 와이어 형태 그대로 접어 넣어 피어의 계산과 일치시킴
[[nodiscard]] UInt32 FrameChecksum( UInt16 opcode, UInt32 seq,
                                    std::span< const Byte > payload ) noexcept;

// out 뒤에 프레임 1개를 덧붙인다
// 페이로드가 kMaxPayload 를 넘으면 out 을 건드리지 않고 false
// 상한을 넘긴 호출자는 와이어로 표현할 수 없는 버그
bool EncodeFrame( std::vector< Byte >& out, UInt16 opcode, UInt32 seq,
                  std::span< const Byte > payload );

// in 에서 고정 헤더를 읽음. kFrameHeaderSize 미만이면 false
// [CS 4.4] 크기 검사가 크기 산술보다 먼저
// 잘린 버퍼에 부호 없는 뺄셈을 걸면 값이 거대하게 뒤집힌다
bool DecodeFrameHeader( std::span< const Byte > in, FrameHeader& out ) noexcept;

}  // namespace atlas
