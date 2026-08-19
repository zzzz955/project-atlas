#pragma once

// =============================================================================
// 세션이 넘긴 바이트 스트림을 온전한 프레임으로 재조립
// 리더 1개 = 세션 1개. throw 하지 않고 실패를 반환값으로 돌려줌
// =============================================================================

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "atlas/core/types.h"
#include "atlas/proto/frame.h"

namespace atlas
{

// [AD 8.1] 프레임 거부 사유. None 외의 값은 전부 연결 종료를 뜻함
// 조용히 버리면 프로토콜이 소리 없이 깨지고 증상은 며칠 뒤 엉뚱한 곳에서 나옴
enum class FrameError : UInt8
{
    None = 0,
    PayloadTooLarge,     // 선언 길이가 kMaxPayload 초과
    ChecksumMismatch,    // crc32 불일치. 프레이밍 어긋남 또는 바이트 손상
    SequenceRegression,  // seq 역행 또는 중복
};

// [AD 11.2a] 아무것도 throw 하지 않는다
// 잘렸거나 악의적인 패킷은 공개 소켓의 평범한 입력이지 예외가 아니다
// [AD 9.1] 스레드 안전 아님. 리더는 세션 1개 소유이고 그 strand 위에서만 접근
class FrameReader
{
public:
    // 잔여분 뒤에 bytes 를 붙이고 완성된 프레임을 도착 순서대로 out 에 적재
    // None 은 스트림 정상(완성된 프레임이 없어 부분 프레임만 남은 경우 포함).
    // 그 외 값이면 호출자가 연결을 닫을 것. 에러 직전까지 디코드된 프레임은 out 에 남음
    FrameError Feed( std::span< const Byte > bytes, std::vector< Frame >& out );

    // 프레임 완성을 기다리며 붙들고 있는 바이트 수
    // 상한은 kFrameHeaderSize + kMaxPayload + 소켓 읽기 1회분
    [[nodiscard]] std::size_t Pending() const noexcept;

    [[nodiscard]] static std::string_view Describe( FrameError error ) noexcept;

private:
    // 소비한 앞부분을 압축. 프레임마다가 아니라 Feed 1회당 1번 호출
    // 프레임 여러 개를 실어 온 읽기가 memmove 를 개수만큼 물지 않는다
    void DropConsumed();

    std::vector< Byte > buffer_;
    std::size_t cursor_{ 0 };
    UInt32 last_seq_{ 0 };
    bool has_last_seq_{ false };
};

}  // namespace atlas
