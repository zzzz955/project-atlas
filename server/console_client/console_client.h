#pragma once

// =============================================================================
// 데모를 손으로 조작하는 대화형 콘솔 클라이언트
// 프레임 계층/생성 코덱/오프코드 표를 재사용. 자체 인코딩 없음
// =============================================================================

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "atlas/core/types.h"
#include "atlas/net/net_types.h"
#include "atlas/proto/frame.h"
#include "atlas/proto/frame_reader.h"

// [AD 16.1a] 두 번째 프레이밍 구현은 자기 버그를 달고 온다
// 그때부터 데모는 디버깅 세션이 된다
// [AD 9.1] 동시성은 서버와 같은 모델 - io_context 1개, 연결당 strand 1개.
// [AD 3.2] 렌더링 없음 - tick 루프도 AoI 도 없어 그릴 상태가 없음

namespace atlas_console
{

// 왕복 종료 또는 연결 사망 시 연결 strand 위에서 실행. 결과를 싣지 않음.
// 안 불리면 터미널이 멈추므로 Close() 도 대기 중인 것을 발화시킴
using Completion = std::function< void() >;

// 연결 1개를 요청 1개씩 순차로 구동. 항상 1개만 in-flight
// REPL 스레드가 done 을 기다린 뒤 프롬프트를 띄운다
// 그래서 출력과 입력이 섞이지 않는다. 멤버는 전부 이 strand 전용
class ConsoleClient : public std::enable_shared_from_this< ConsoleClient >
{
public:
    ConsoleClient( atlas::IoContext& io_context, atlas::Endpoint endpoint );

    ConsoleClient( const ConsoleClient& ) = delete;
    ConsoleClient& operator=( const ConsoleClient& ) = delete;
    ConsoleClient( ConsoleClient&& ) = delete;
    ConsoleClient& operator=( ConsoleClient&& ) = delete;
    ~ConsoleClient() = default;

    // 아래 진입점은 전부 아무 스레드에서나 호출 가능. 각자 strand 로 post 한다
    // done 은 응답 도착(또는 연결 끊김) 시점에 strand 위에서 실행된다
    void Connect( Completion done );
    void RequestLoad( atlas::UInt64 character_id, Completion done );
    // RequestLoad 와 같은 오프코드 - 인벤토리를 싣는 응답이 이것뿐임.
    // [AD 10.2] equip 직후 호출하면 캐시가 무효화된 뒤라 cold read 경로가 드러난다
    void RequestInventory( Completion done );
    void RequestEquip( atlas::UInt64 item_uid, atlas::UInt8 slot, Completion done );
    void RequestRanking( atlas::UInt16 count, Completion done );

    void Shutdown();

    // REPL 스레드가 읽으므로 atomic
    [[nodiscard]] bool Alive() const noexcept { return !closed_.load(); }

private:
    void OnConnect( const atlas::ErrorCode& error );
    void ReadMore();
    void OnRead( const atlas::ErrorCode& error, std::size_t bytes_read );
    void OnFrame( const atlas::Frame& frame );

    void PrintLoadResponse( const atlas::Frame& frame );
    void PrintEquipResponse( const atlas::Frame& frame );
    void PrintRankingResponse( const atlas::Frame& frame );

    // 시작할 왕복의 완료 콜백을 설치한다
    // 아직 남은 완료가 있으면 먼저 발화시킨다
    // 그냥 버리면 REPL 스레드가 영원히 대기한다
    void BeginRequest( Completion done );
    void FinishRequest();

    void SendFrame( atlas::UInt16 opcode );
    void OnTransportError();
    void Close();

    atlas::Strand strand_;
    atlas::Socket socket_;
    atlas::Endpoint endpoint_;

    atlas::FrameReader reader_;
    std::vector< atlas::Frame > frames_;
    std::array< atlas::Byte, 4096 > read_buffer_{};
    std::vector< atlas::Byte > payload_;
    std::vector< atlas::Byte > wire_;

    Completion pending_done_;
    // 대기 중인 응답. 0 = 없음이며 유효 오프코드가 아니라 충돌하지 않음
    atlas::UInt16 awaited_opcode_{ 0 };
    bool items_only_{ false };

    // [AD 8.2] 서버가 연결에서 신원을 직접 도출한다
    // 이 사본은 권한 근거가 아니라 inv 명령의 편의용
    atlas::UInt64 character_id_{ 0 };
    bool loaded_{ false };

    // [AD 8.3] 연결당 송신 카운터
    // 코어 SendFrame 에 숨기지 않고 무엇을 보낼지 정하는 계층이 소유한다
    atlas::UInt32 send_seq_{ 0 };
    std::atomic< bool > closed_{ false };
};

}  // namespace atlas_console
