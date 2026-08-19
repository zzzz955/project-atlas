#pragma once

// =============================================================================
// CRC-32 (IEEE 802.3, reflected, 0xEDB88320). 프레이밍 무결성 전용
// 검사 구간이 헤더 일부 + 페이로드의 가상 연결이라 스트리밍 형태
// =============================================================================

#include <span>

#include "atlas/core/types.h"

namespace atlas
{

inline constexpr UInt32 kCrc32Init = 0xFFFFFFFFU;  // 끝에 Crc32Finish 1회

// [AD 8.2] 변조 방지가 아니다
// 알고리즘이 공개라 고친 쪽이 체크섬을 다시 계산하면 그대로 통과한다
// 사는 이유는 길이 접두사가 프레임 경계와 어긋난 스트림을 잡는 것
// 그것도 첫 프레임에서 잡는다. 변조 차단은 세션 키 HMAC
[[nodiscard]] UInt32 Crc32Update( UInt32 state, std::span< const Byte > data ) noexcept;

[[nodiscard]] UInt32 Crc32Finish( UInt32 state ) noexcept;

}  // namespace atlas
