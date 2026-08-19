// =============================================================================
// FrameReader 구현. 재조립 버퍼, 검증 순서, 소비분 압축
// =============================================================================

#include "atlas/proto/frame_reader.h"

#include <utility>

namespace atlas
{

FrameError FrameReader::Feed( std::span< const Byte > bytes, std::vector< Frame >& out )
{
    buffer_.insert( buffer_.end(), bytes.begin(), bytes.end() );

    for ( ;; )
    {
        const std::size_t available = buffer_.size() - cursor_;
        if ( available < kFrameHeaderSize )
        {
            break;  // 헤더가 읽기 사이에 잘림. 들고 나머지를 기다림
        }

        const std::span< const Byte > rest =
            std::span< const Byte >( buffer_ ).subspan( cursor_, available );

        FrameHeader header{};
        if ( !DecodeFrameHeader( rest, header ) )
        {
            break;  // 위 검사로 도달 불가. 계약을 가정으로 지우지 않으려 남김
        }

        const auto declared = static_cast< std::size_t >( header.length );

        // [CS 4.4] 검증 순서가 핵심. 상한 -> 잔여 크기 -> subspan
        // 접두사는 피어가 고르므로 거짓말이 할당까지 도달하면 안 됨
        if ( declared > kMaxPayload )
        {
            return FrameError::PayloadTooLarge;
        }
        // 위에서 available >= kFrameHeaderSize 가 확정되어 이 뺄셈은 뒤집히지 않음
        if ( available - kFrameHeaderSize < declared )
        {
            break;  // 정직한 접두사인데 페이로드가 덜 도착. 에러 아님
        }

        const std::span< const Byte > payload = rest.subspan( kFrameHeaderSize, declared );

        if ( FrameChecksum( header.opcode, header.seq, payload ) != header.crc32 )
        {
            return FrameError::ChecksumMismatch;
        }

        // [AD 8.3] 세션당 수신 카운터 1개
        // 중복도 역행도 리플레이이거나 어긋난 스트림이다
        // 어느 쪽이든 연결 종료
        if ( has_last_seq_ && header.seq <= last_seq_ )
        {
            return FrameError::SequenceRegression;
        }
        last_seq_ = header.seq;
        has_last_seq_ = true;

        Frame frame;
        frame.opcode = header.opcode;
        frame.seq = header.seq;
        frame.payload.assign( payload.begin(), payload.end() );
        out.push_back( std::move( frame ) );

        cursor_ += kFrameHeaderSize + declared;
    }

    DropConsumed();
    return FrameError::None;
}

std::size_t FrameReader::Pending() const noexcept { return buffer_.size() - cursor_; }

std::string_view FrameReader::Describe( FrameError error ) noexcept
{
    switch ( error )
    {
        case FrameError::None:
            return "none";
        case FrameError::PayloadTooLarge:
            return "payload above kMaxPayload";
        case FrameError::ChecksumMismatch:
            return "crc32 mismatch";
        case FrameError::SequenceRegression:
            return "seq repeated or went backwards";
    }
    return "unknown";
}

void FrameReader::DropConsumed()
{
    if ( cursor_ == 0 )
    {
        return;
    }
    buffer_.erase( buffer_.begin(), buffer_.begin() + static_cast< std::ptrdiff_t >( cursor_ ) );
    cursor_ = 0;
}

}  // namespace atlas
