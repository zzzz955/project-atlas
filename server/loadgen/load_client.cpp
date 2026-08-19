// =============================================================================
// 부하 하네스 실행부. 연결마다 strand 1개로 요청/응답 핑퐁을 돌린다
// 결과는 단계별로 모아 LoadStats 로 합친다
// =============================================================================

#include "loadgen/load_client.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/error.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/net/net_types.h"
#include "atlas/proto/frame.h"
#include "atlas/proto/frame_reader.h"
#include "game/equip_service.h"
#include "game/handlers.h"
#include "game/inventory.h"
#include "generated/pkt/pkt_codec.h"
#include "loadgen/live_view.h"

namespace atlas_loadgen
{

namespace
{

using atlas::Byte;
using atlas::Int64;
using atlas::UInt16;
using atlas::UInt32;
using atlas::UInt64;
using atlas::UInt8;

// 소켓 읽기 1회분. 재조립 상한은 이 버퍼가 아니라 FrameReader 가 정함
constexpr std::size_t kReadBufferSize = 4096;

// 실행 마감 이후 워치독 유예. 이때까지 안 돌아온 연결은 느린 게 아니라 멈춘 것
constexpr atlas::Seconds kWatchdogGrace{ 10 };

// [AD 10.5] 요청마다 2개 테이블에 같은 3회 쓰기가 발생.
// 시드된 아이템 2개가 번갈아 들어가 대상 슬롯이 항상 점유 상태
constexpr atlas_demo::EquipSlot kDrivenSlot = atlas_demo::EquipSlot::Weapon;

// [AD 10.8] 로드가 거절된 뒤 재요청까지의 대기
// 방금 셰딩된 버스트에 재시도가 얹히지 않을 만큼 길다
// 합류 연결이 단계 정상 구간에 닿을 만큼 짧다
constexpr atlas::Millis kLoadRetryDelay{ 250 };

// 단계 1개의 시계. 고정 동시성 실행은 목록 길이가 1일 뿐 같은 경로
struct StageWindow
{
    std::size_t connections{ 0 };
    atlas::TimePoint begin{};
    // [AD 16.1a] 이 시점 이전 표본은 폐기한다
    // connect, 첫 캐릭터 로드, 드라이버의 첫 prepare 가 여기 들어간다
    // 램프에서는 단계마다 다시 들어간다
    atlas::TimePoint steady_begin{};
    atlas::TimePoint end{};
};

struct StageSlice
{
    std::size_t responses_ok{ 0 };
    std::size_t responses_rejected{ 0 };
    std::size_t responses_refused{ 0 };
    std::vector< UInt32 > latencies_us;
    std::vector< UInt32 > ok_latencies_us;
};

// 연결 1개가 관측한 것. io_context 가 비워진 뒤 병합하므로 락이 필요 없음
struct ConnectionResult
{
    bool established{ false };
    bool connect_failed{ false };
    bool transport_failed{ false };
    bool load_failed{ false };
    std::size_t requests_sent{ 0 };
    std::size_t responses_ok{ 0 };
    std::size_t responses_unavailable{ 0 };
    std::size_t responses_refused{ 0 };
    std::size_t loads_rejected{ 0 };
    // 요청한 소켓 옵션이 걸리지 않음. 라벨과 반대를 측정한 셀은 없느니만 못함
    bool no_delay_failed{ false };
    std::vector< StageSlice > stages;
};

// 연결 1개 - connect, 캐릭터 로드, 이후 마감까지 equip 요청/응답 핑퐁
// [AD 9.1] 연결당 strand 1개로 서버와 동일하다
// 요청이 1개뿐이라 정확성에는 필요 없다
// 클라이언트도 스레드 농장이 아님을 보이는 것이 하네스의 논점
class LoadConnection : public std::enable_shared_from_this< LoadConnection >
{
public:
    LoadConnection( atlas::IoContext& io_context, atlas::Endpoint endpoint, UInt64 character_id,
                    atlas::Duration interval, atlas::TimePoint first_send,
                    atlas::TimePoint activate_at, const std::vector< StageWindow >& windows,
                    std::optional< bool > no_delay, std::atomic< std::size_t >& live,
                    std::atomic< std::size_t >& peak, LiveMetrics* metrics,
                    std::function< void() > on_finished )
        : strand_( atlas::asio::make_strand( io_context ) ),
          socket_( strand_ ),
          timer_( strand_ ),
          endpoint_( std::move( endpoint ) ),
          character_id_( character_id ),
          interval_( interval ),
          // ScheduleNext 가 대기 전에 1간격 전진하므로 첫 송신이 정확히 first_send
          next_send_( first_send - interval ),
          activate_at_( activate_at ),
          windows_( windows ),
          no_delay_( no_delay ),
          deadline_( windows.back().end ),
          live_( live ),
          peak_( peak ),
          metrics_( metrics ),
          on_finished_( std::move( on_finished ) )
    {
        result_.stages.resize( windows.size() );
    }

    LoadConnection( const LoadConnection& ) = delete;
    LoadConnection& operator=( const LoadConnection& ) = delete;
    LoadConnection( LoadConnection&& ) = delete;
    LoadConnection& operator=( LoadConnection&& ) = delete;
    ~LoadConnection() = default;

    // 늦은 단계에 합류할 연결은 여기서 대기한다
    // 전부 미리 열어 두면 적용하지도 않은 램프를 보고하게 된다
    void Start()
    {
        if ( activate_at_ <= atlas::Clock::now() )
        {
            Connect();
            return;
        }
        auto self = shared_from_this();
        timer_.expires_at( activate_at_ );
        timer_.async_wait( atlas::asio::bind_executor(
            strand_, atlas::Guarded( atlas::Ctx{},
                                     [self]( const atlas::ErrorCode& error )
                                     {
                                         if ( error || self->closed_ )
                                         {
                                             return;
                                         }
                                         self->Connect();
                                     } ) ) );
    }

    // 아무 스레드에서나 호출 가능 - 워치독이 사용
    void Stop()
    {
        auto self = shared_from_this();
        atlas::asio::post( strand_, atlas::Guarded( atlas::Ctx{}, [self] { self->Close(); } ) );
    }

    [[nodiscard]] const ConnectionResult& Result() const noexcept { return result_; }

private:
    void Connect()
    {
        auto self = shared_from_this();
        socket_.async_connect(
            endpoint_,
            atlas::asio::bind_executor(
                strand_, atlas::Guarded( atlas::Ctx{}, [self]( const atlas::ErrorCode& error )
                                         { self->OnConnect( error ); } ) ) );
    }

    void OnConnect( const atlas::ErrorCode& error )
    {
        if ( error )
        {
            result_.connect_failed = true;
            NoteError();
            Finish();
            return;
        }
        result_.established = true;
        // [AD 16.1] 요청이 있을 때만 건다
        // 손대지 않은 소켓이 기존 표의 조건이다
        // 조용히 옵션을 걸기 시작하면 비교 가능성이 사라진다
        if ( no_delay_.has_value() )
        {
            // 동기 오버로드가 양쪽으로 보고하므로 명시적으로 버림. 치명적이지 않음
            atlas::ErrorCode nodelay_ec;
            std::ignore = socket_.set_option( atlas::Tcp::no_delay( *no_delay_ ), nodelay_ec );
            if ( nodelay_ec )
            {
                result_.no_delay_failed = true;
            }
        }
        BumpLive();
        ReadMore();
        SendLoadRequest();
    }

    void ReadMore()
    {
        auto self = shared_from_this();
        socket_.async_read_some(
            atlas::asio::buffer( read_buffer_ ),
            atlas::asio::bind_executor(
                strand_, atlas::Guarded( atlas::Ctx{}, [self]( const atlas::ErrorCode& error,
                                                               std::size_t bytes_read )
                                         { self->OnRead( error, bytes_read ); } ) ) );
    }

    void OnRead( const atlas::ErrorCode& error, std::size_t bytes_read )
    {
        if ( closed_ )
        {
            return;
        }
        if ( error )
        {
            OnTransportError();
            return;
        }

        frames_.clear();
        const atlas::FrameError framing =
            reader_.Feed( std::span< const Byte >( read_buffer_.data(), bytes_read ), frames_ );
        for ( const atlas::Frame& frame : frames_ )
        {
            OnFrame( frame );
        }
        if ( framing != atlas::FrameError::None )
        {
            OnTransportError();
            return;
        }
        if ( closed_ )
        {
            return;
        }
        ReadMore();
    }

    void OnFrame( const atlas::Frame& frame )
    {
        if ( closed_ )
        {
            return;
        }
        if ( frame.opcode == atlas_demo::kOpCharacterLoadResponse )
        {
            OnLoadResponse( frame );
            return;
        }
        if ( frame.opcode == atlas_demo::kOpEquipResponse )
        {
            OnEquipResponse( frame );
            return;
        }
        // 요청하지 않은 오프코드 = 프로토콜 인식 불일치. 추측하지 않고 중단
        OnTransportError();
    }

    void OnLoadResponse( const atlas::Frame& frame )
    {
        std::span< const Byte > cursor( frame.payload );
        UInt8 result = 0xFF;
        if ( !atlas::generated::ReadLe( cursor, result ) )
        {
            result_.load_failed = true;
            NoteError();
            Close();
            return;
        }
        // [AD 10.8] 거절된 로드는 유실된 연결이 아니다
        // 서버는 소켓을 열어 둔 채 Unavailable 로 답한다
        // 정직한 대응은 재요청이다
        // 여기서 닫으면 부하 구간에서 연결이 빠져 표가 뒤집힌다
        if ( result == static_cast< UInt8 >( atlas_demo::LoadResult::Unavailable ) )
        {
            ++result_.loads_rejected;
            NoteLoadRejection();
            RetryLoadLater();
            return;
        }
        if ( result != static_cast< UInt8 >( atlas_demo::LoadResult::Ok ) )
        {
            result_.load_failed = true;
            NoteError();
            Close();
            return;
        }

        UInt64 loaded_character = 0;
        UInt64 account_uid = 0;
        std::string name;
        atlas::Int32 pos_x = 0;
        atlas::Int32 pos_y = 0;
        UInt16 level = 0;
        UInt64 exp = 0;
        UInt16 item_count = 0;
        const bool decoded = atlas::generated::ReadLe( cursor, loaded_character ) &&
                             atlas::generated::ReadLe( cursor, account_uid ) &&
                             atlas::generated::ReadUtf8( cursor, name ) &&
                             atlas::generated::ReadLe( cursor, pos_x ) &&
                             atlas::generated::ReadLe( cursor, pos_y ) &&
                             atlas::generated::ReadLe( cursor, level ) &&
                             atlas::generated::ReadLe( cursor, exp ) &&
                             atlas::generated::ReadLe( cursor, item_count );
        if ( !decoded )
        {
            result_.load_failed = true;
            NoteError();
            Close();
            return;
        }

        std::size_t taken = 0;
        for ( UInt16 index = 0; index < item_count; ++index )
        {
            UInt64 item_uid = 0;
            UInt32 item_id = 0;
            UInt16 stack_count = 0;
            UInt8 slot = 0;
            if ( !atlas::generated::ReadLe( cursor, item_uid ) ||
                 !atlas::generated::ReadLe( cursor, item_id ) ||
                 !atlas::generated::ReadLe( cursor, stack_count ) ||
                 !atlas::generated::ReadLe( cursor, slot ) )
            {
                break;
            }
            if ( taken < item_uids_.size() )
            {
                item_uids_[taken] = item_uid;
                ++taken;
            }
        }

        // [AD 10.5] 아이템 2개가 시나리오 자체다
        // 한 슬롯을 번갈아 차지해야 매 요청이 실제 점유자를 해제한다
        // 그래야 쓰기 3회가 모두 발생한다
        if ( taken < item_uids_.size() )
        {
            result_.load_failed = true;
            NoteError();
            Close();
            return;
        }

        ScheduleNext();
    }

    void OnEquipResponse( const atlas::Frame& frame )
    {
        const atlas::TimePoint arrived = atlas::Clock::now();

        std::span< const Byte > cursor( frame.payload );
        UInt8 code = atlas_demo::kEquipResponseUnavailable;
        if ( !atlas::generated::ReadLe( cursor, code ) )
        {
            OnTransportError();
            return;
        }

        // [AD 10.8] 결과는 셋이다
        // 거부(큐에 못 넣어 사절)와 거절(게임 규칙상 불가)은 따로 센다
        // 합치면 안 망가진 실행을 "망가졌다" 로 보고하게 된다
        const bool rejected = code == atlas_demo::kEquipResponseUnavailable;
        const bool succeeded = code == static_cast< UInt8 >( atlas_demo::EquipResult::Ok );
        if ( rejected )
        {
            ++result_.responses_unavailable;
            NoteRejection();
        }
        else if ( succeeded )
        {
            ++result_.responses_ok;
        }
        else
        {
            ++result_.responses_refused;
            NoteError();
        }

        const UInt32 elapsed_us = static_cast< UInt32 >(
            std::chrono::duration_cast< atlas::Micros >( arrived - sent_at_ ).count() );

        // 정상 구간만 표본화한다
        // 워밍업에 들어가는 것들은 실행당 1회 비용이다
        // 그것이 짧은 실행의 꼬리를 끌어내린다
        const std::size_t stage = StageIndexAt( arrived );
        const StageWindow& window = windows_[stage];
        if ( arrived >= window.steady_begin && arrived <= window.end )
        {
            StageSlice& slice = result_.stages[stage];
            slice.latencies_us.push_back( elapsed_us );
            if ( rejected )
            {
                ++slice.responses_rejected;
            }
            else if ( succeeded )
            {
                ++slice.responses_ok;
                slice.ok_latencies_us.push_back( elapsed_us );
            }
            else
            {
                ++slice.responses_refused;
            }
        }
        // [AD 16.1c] 라이브 뷰는 워밍업 포함 전 구간을 본다
        // 폐기된 앞부분이 가리는 전이가 정확히 보여야 할 것이다
        // 별도 히스토그램이며 보고값에 관여하지 않는다
        if ( metrics_ != nullptr )
        {
            metrics_->RecordLatency( elapsed_us, succeeded );
        }

        ScheduleNext();
    }

    void ScheduleNext()
    {
        if ( atlas::Clock::now() >= deadline_ )
        {
            Close();
            return;
        }
        if ( interval_ == atlas::Duration::zero() )
        {
            SendEquipRequest();
            return;
        }

        // 페이싱은 now 가 아니라 고정 보폭으로 전진한다
        // 느린 응답이 남은 실행의 부하율을 조용히 낮추지 않는다
        // now 로 클램프해 몰아치기도 막는다
        const atlas::TimePoint now = atlas::Clock::now();
        next_send_ = next_send_ + interval_;
        if ( next_send_ < now )
        {
            next_send_ = now;
        }

        auto self = shared_from_this();
        timer_.expires_at( next_send_ );
        timer_.async_wait( atlas::asio::bind_executor(
            strand_, atlas::Guarded( atlas::Ctx{},
                                     [self]( const atlas::ErrorCode& error )
                                     {
                                         if ( error || self->closed_ )
                                         {
                                             return;
                                         }
                                         if ( atlas::Clock::now() >= self->deadline_ )
                                         {
                                             self->Close();
                                             return;
                                         }
                                         self->SendEquipRequest();
                                     } ) ) );
    }

    void SendEquipRequest()
    {
        const UInt64 item_uid = item_uids_[next_item_];
        next_item_ = ( next_item_ + 1U ) % item_uids_.size();

        payload_.clear();
        atlas::generated::WriteLe( payload_, item_uid );
        atlas::generated::WriteLe( payload_, static_cast< UInt8 >( kDrivenSlot ) );
        ++result_.requests_sent;
        SendFrame( atlas_demo::kOpEquipRequest );
    }

    void SendLoadRequest()
    {
        payload_.clear();
        atlas::generated::WriteLe( payload_, character_id_ );
        SendFrame( atlas_demo::kOpCharacterLoadRequest );
    }

    // [AD 8.3] seq 는 코어가 아니라 여기가 소유한다
    // 연결당 송신 카운터는 무엇을 보낼지 정하는 계층이 가진다
    void SendFrame( UInt16 opcode )
    {
        // 핑퐁이 보장하는 것은 요청 1개이지 쓰기 1개가 아니다
        // async_write 완료 전에는 wire_ 를 덮어쓸 수 없다
        // 큐 상한을 넘으면 거부가 us 로 돌아와 읽기 완료가 먼저 도달한다
        // 그러면 다음 요청이 살아 있는 쓰기의 버퍼를 덮어쓴다
        // session.cpp 가 페이로드를 큐에 물려 두는 것과 같은 규칙
        if ( write_in_flight_ )
        {
            deferred_opcode_ = opcode;
            return;
        }

        ++send_seq_;
        wire_.clear();
        if ( !atlas::EncodeFrame( wire_, opcode, send_seq_, payload_ ) )
        {
            OnTransportError();
            return;
        }

        sent_at_ = atlas::Clock::now();
        write_in_flight_ = true;
        auto self = shared_from_this();
        atlas::asio::async_write(
            socket_, atlas::asio::buffer( wire_ ),
            atlas::asio::bind_executor(
                strand_, atlas::Guarded( atlas::Ctx{},
                                         [self]( const atlas::ErrorCode& error, std::size_t )
                                         {
                                             self->write_in_flight_ = false;
                                             if ( error && !self->closed_ )
                                             {
                                                 self->OnTransportError();
                                                 return;
                                             }
                                             // 위 구간에 도착해 밀린 요청.
                                             // payload_ 는 아직 그 본문을 그대로 가짐
                                             if ( self->deferred_opcode_.has_value() &&
                                                  !self->closed_ )
                                             {
                                                 const UInt16 opcode = *self->deferred_opcode_;
                                                 self->deferred_opcode_.reset();
                                                 self->SendFrame( opcode );
                                             }
                                         } ) ) );
    }

    void OnTransportError()
    {
        if ( closed_ )
        {
            return;
        }
        if ( result_.established )
        {
            result_.transport_failed = true;
            NoteError();
        }
        Close();
    }

    void Close()
    {
        if ( closed_ )
        {
            return;
        }
        closed_ = true;

        // 반환값만으로는 부족해 명시적으로 버림
        atlas::ErrorCode ignored;
        std::ignore = socket_.shutdown( atlas::Socket::shutdown_both, ignored );
        std::ignore = socket_.close( ignored );
        timer_.cancel();

        if ( result_.established )
        {
            live_.fetch_sub( 1 );
        }
        Finish();
    }

    void Finish()
    {
        if ( finished_ )
        {
            return;
        }
        finished_ = true;
        if ( on_finished_ )
        {
            on_finished_();
        }
    }

    // arrived 가 속한 단계 = 이미 열린 마지막 창. 단계가 몇 개뿐이라 선형 탐색
    [[nodiscard]] std::size_t StageIndexAt( atlas::TimePoint arrived ) const noexcept
    {
        std::size_t index = 0;
        for ( std::size_t candidate = 0; candidate < windows_.size(); ++candidate )
        {
            if ( windows_[candidate].begin <= arrived )
            {
                index = candidate;
            }
        }
        return index;
    }

    void RetryLoadLater()
    {
        if ( atlas::Clock::now() >= deadline_ )
        {
            Close();
            return;
        }
        auto self = shared_from_this();
        timer_.expires_after( kLoadRetryDelay );
        timer_.async_wait( atlas::asio::bind_executor(
            strand_, atlas::Guarded( atlas::Ctx{},
                                     [self]( const atlas::ErrorCode& error )
                                     {
                                         if ( error || self->closed_ )
                                         {
                                             return;
                                         }
                                         if ( atlas::Clock::now() >= self->deadline_ )
                                         {
                                             self->Close();
                                             return;
                                         }
                                         self->SendLoadRequest();
                                     } ) ) );
    }

    void NoteError() noexcept
    {
        if ( metrics_ != nullptr )
        {
            metrics_->RecordError();
        }
    }

    // 의도적으로 NoteError 가 아니다
    // 거부 선이 오르는 동안 에러 선은 0 으로 남는다
    // 그 대비가 관측 결과 자체다
    void NoteRejection() noexcept
    {
        if ( metrics_ != nullptr )
        {
            metrics_->RecordRejection();
        }
    }

    void NoteLoadRejection() noexcept
    {
        if ( metrics_ != nullptr )
        {
            metrics_->RecordLoadRejection();
        }
    }

    void BumpLive()
    {
        const std::size_t live = live_.fetch_add( 1 ) + 1;
        std::size_t seen = peak_.load();
        while ( live > seen && !peak_.compare_exchange_weak( seen, live ) )
        {
        }
    }

    atlas::Strand strand_;
    atlas::Socket socket_;
    atlas::SteadyTimer timer_;
    atlas::Endpoint endpoint_;

    UInt64 character_id_{ 0 };
    atlas::Duration interval_{};
    // 다음 페이싱 송신 시각
    // 연결마다 시작 위상을 달리 주어 합산 부하가 고르게 도착한다
    // 없으면 전부 같은 tick 에 발사해 자기 thundering herd 를 측정한다
    // (실측: p50 53 ms 가 위상 분산 후 5 ms)
    atlas::TimePoint next_send_{};
    atlas::TimePoint activate_at_{};
    // RunLoad 소유이며 모든 연결보다 오래 삶. 구성 후에는 읽기 전용
    const std::vector< StageWindow >& windows_;
    std::optional< bool > no_delay_;
    atlas::TimePoint deadline_{};
    atlas::TimePoint sent_at_{};

    std::atomic< std::size_t >& live_;
    std::atomic< std::size_t >& peak_;
    // [AD 16.1a] 라이브 뷰를 요청하지 않으면 null
    // 기본 측정 경로에서 관측 축의 비용은 이 null 검사 하나가 전부
    LiveMetrics* metrics_{ nullptr };
    std::function< void() > on_finished_;

    atlas::FrameReader reader_;
    std::vector< atlas::Frame > frames_;
    std::array< Byte, kReadBufferSize > read_buffer_{};
    std::vector< Byte > payload_;
    std::vector< Byte > wire_;
    // true 인 동안 wire_ 는 진행 중인 쓰기의 소유 - SendFrame 참고
    bool write_in_flight_{ false };
    std::optional< UInt16 > deferred_opcode_;

    std::array< UInt64, 2 > item_uids_{};
    std::size_t next_item_{ 0 };
    UInt32 send_seq_{ 0 };
    bool closed_{ false };
    bool finished_{ false };

    ConnectionResult result_;
};

}  // namespace

LoadStats RunLoad( const LoadOptions& options )
{
    LoadStats stats;
    stats.connections_attempted = options.connections;
    if ( options.connections == 0 )
    {
        return stats;
    }

    atlas::IoContext io_context;
    const atlas::Endpoint endpoint( atlas::asio::ip::make_address( options.host ), options.port );

    const atlas::TimePoint start = atlas::Clock::now();
    const atlas::TimePoint deadline = start + atlas::Seconds{ options.duration_seconds };

    // [AD 16.1] 고정 동시성이면 전 구간을 덮는 창 1개
    // 기존 표의 채택 규칙을 두 벌이 아니라 한 벌로 표현한 것
    std::vector< StageWindow > windows;
    if ( options.ramp_stages.empty() )
    {
        windows.push_back(
            StageWindow{ .connections = options.connections,
                         .begin = start,
                         .steady_begin = start + atlas::Seconds{ options.warmup_seconds },
                         .end = deadline } );
    }
    else
    {
        for ( std::size_t index = 0; index < options.ramp_stages.size(); ++index )
        {
            const atlas::TimePoint begin =
                start + ( atlas::Seconds{ options.stage_seconds } * static_cast< Int64 >( index ) );
            windows.push_back(
                StageWindow{ .connections = options.ramp_stages[index],
                             .begin = begin,
                             .steady_begin = begin + atlas::Seconds{ options.warmup_seconds },
                             .end = begin + atlas::Seconds{ options.stage_seconds } } );
        }
    }

    UInt64 steady_total_ms = 0;
    for ( const StageWindow& window : windows )
    {
        steady_total_ms += static_cast< UInt64 >(
            std::chrono::duration_cast< atlas::Millis >( window.end - window.steady_begin )
                .count() );
    }
    stats.steady_window_ms = static_cast< UInt32 >( steady_total_ms );

    // 목표 R rps 를 N 연결에 나누면 연결당 N/R 초에 1회
    atlas::Duration interval = atlas::Duration::zero();
    if ( options.rate_per_second != 0 )
    {
        interval = atlas::Micros{ ( 1000000ULL * static_cast< UInt64 >( options.connections ) ) /
                                  options.rate_per_second };
    }

    std::atomic< std::size_t > live{ 0 };
    std::atomic< std::size_t > peak{ 0 };
    std::atomic< std::size_t > remaining{ options.connections };
    std::atomic< bool > watchdog_fired{ false };

    // [AD 16.1a] 두 플래그가 없으면 아래 어떤 것도 할당/기동/검사되지 않음.
    // 기본 실행은 기존 측정과 동일
    const bool live_view_wanted = options.tui || !options.sample_jsonl_path.empty();
    std::unique_ptr< LiveMetrics > metrics;
    std::unique_ptr< LiveView > live_view;
    if ( live_view_wanted )
    {
        metrics = std::make_unique< LiveMetrics >();
        LiveViewOptions view_options;
        view_options.tui = options.tui;
        view_options.jsonl_path = options.sample_jsonl_path;
        view_options.probe_file_path = options.probe_file_path;
        // [AD 16.1b] 실행 조건은 표본과 함께 이동한다
        // 실행이 스스로 알 수 없는 조건은 리포트에 주석으로 전달한다
        // 조건 없는 곡선은 조건 없는 표만큼 못 읽는다
        std::string ramp;
        for ( const std::size_t stage_connections : options.ramp_stages )
        {
            ramp += ramp.empty() ? "" : ",";
            ramp += std::to_string( stage_connections );
        }
        view_options.meta_json = std::format(
            R"({{"kind":"meta","harness":"atlas_loadgen","connections":{},"rate_per_second":{},)"
            R"("duration_seconds":{},"warmup_seconds":{},"io_threads":{},"host":"{}","port":{},)"
            R"("server_id":{},"first_character_id":{},"ramp":"{}","stage_seconds":{},)"
            R"("sample_interval_ms":1000}})",
            options.connections, options.rate_per_second, options.duration_seconds,
            options.warmup_seconds, options.io_threads, options.host, options.port,
            options.server_id, options.first_character_id, ramp, options.stage_seconds );
        for ( const StageWindow& window : windows )
        {
            view_options.stages.push_back( StageMark{
                .begins_at_seconds =
                    static_cast< atlas::Float64 >(
                        std::chrono::duration_cast< atlas::Millis >( window.begin - start )
                            .count() ) /
                    1000.0,
                .connections = window.connections } );
        }
        live_view = std::make_unique< LiveView >( std::move( view_options ), *metrics, live );
    }

    atlas::SteadyTimer watchdog( io_context );
    watchdog.expires_at( deadline + kWatchdogGrace );

    // [AD 16.1a] 램프에서 단계가 추가하는 연결은 그 단계 워밍업 구간에 분산한다
    // 동시 connect + 캐릭터 로드는 thundering herd 다
    // 그 형태로 측정 하나를 이미 잃은 적 있다
    // 고정 동시성 실행은 전부 start 그대로
    std::vector< atlas::TimePoint > activations( options.connections, start );
    if ( !options.ramp_stages.empty() )
    {
        const auto spread = std::chrono::duration_cast< atlas::Duration >(
            atlas::Seconds{ options.warmup_seconds } );
        std::size_t already_joined = 0;
        for ( const StageWindow& window : windows )
        {
            const std::size_t joining = window.connections - already_joined;
            for ( std::size_t offset = 0; offset < joining; ++offset )
            {
                activations[already_joined + offset] =
                    window.begin + ( ( spread * static_cast< atlas::Duration::rep >( offset ) ) /
                                     static_cast< atlas::Duration::rep >( joining ) );
            }
            already_joined = window.connections;
        }
    }

    std::vector< std::shared_ptr< LoadConnection > > connections;
    connections.reserve( options.connections );
    for ( std::size_t index = 0; index < options.connections; ++index )
    {
        // 간격 안에서 위상 균등 분산
        // 연결 i 는 앞 연결보다 1/N 보폭 늦게 시작한다
        // 그래서 합산 부하가 버스트가 아니라 정상 스트림으로 도착한다
        const atlas::TimePoint first_send =
            start + ( ( interval * static_cast< atlas::Duration::rep >( index ) ) /
                      static_cast< atlas::Duration::rep >( options.connections ) );
        connections.push_back( std::make_shared< LoadConnection >(
            io_context, endpoint, options.first_character_id + static_cast< UInt64 >( index ),
            interval, first_send, activations[index], windows, options.no_delay, live, peak,
            metrics.get(),
            [&io_context, &remaining, &watchdog]
            {
                if ( remaining.fetch_sub( 1 ) != 1 )
                {
                    return;
                }
                // 마지막 연결이 끝나면 io_context::run() 을 붙드는 것은 워치독뿐.
                // 자기 컨텍스트에서 취소해야 유예 시간을 매번 물지 않음
                atlas::asio::post( io_context, atlas::Guarded( atlas::Ctx{}, [&watchdog]
                                                               { watchdog.cancel(); } ) );
            } ) );
    }

    // 연결이 끝내 답하지 않아도 실행은 끝나야 한다
    // 없으면 서버가 가장 나쁠 때 하네스가 멈춘다
    // 그때가 보고할 가치가 가장 큰 순간이다
    watchdog.async_wait( atlas::Guarded(
        atlas::Ctx{},
        [&watchdog_fired, &connections]( const atlas::ErrorCode& error )
        {
            if ( error )
            {
                return;
            }
            watchdog_fired.store( true );
            for ( const std::shared_ptr< LoadConnection >& connection : connections )
            {
                connection->Stop();
            }
        } ) );

    if ( live_view )
    {
        live_view->Start();
    }

    for ( const std::shared_ptr< LoadConnection >& connection : connections )
    {
        connection->Start();
    }

    const std::size_t thread_count =
        options.io_threads == 0 ? std::size_t{ 1 } : options.io_threads;
    std::vector< std::thread > workers;
    workers.reserve( thread_count );
    for ( std::size_t index = 0; index < thread_count; ++index )
    {
        workers.emplace_back( [&io_context] { io_context.run(); } );
    }
    for ( std::thread& worker : workers )
    {
        worker.join();
    }
    if ( live_view )
    {
        live_view->Stop();
    }

    stats.stages.resize( windows.size() );
    for ( std::size_t index = 0; index < windows.size(); ++index )
    {
        stats.stages[index].connections = windows[index].connections;
        stats.stages[index].window_ms =
            static_cast< UInt32 >( std::chrono::duration_cast< atlas::Millis >(
                                       windows[index].end - windows[index].steady_begin )
                                       .count() );
    }

    for ( const std::shared_ptr< LoadConnection >& connection : connections )
    {
        const ConnectionResult& result = connection->Result();
        stats.connections_established += result.established ? 1U : 0U;
        stats.connect_failures += result.connect_failed ? 1U : 0U;
        stats.transport_failures += result.transport_failed ? 1U : 0U;
        stats.load_failures += result.load_failed ? 1U : 0U;
        stats.requests_sent += result.requests_sent;
        stats.responses_ok += result.responses_ok;
        stats.responses_unavailable += result.responses_unavailable;
        stats.responses_refused += result.responses_refused;
        stats.loads_rejected += result.loads_rejected;
        stats.no_delay_failures += result.no_delay_failed ? 1U : 0U;
        for ( std::size_t index = 0; index < windows.size(); ++index )
        {
            const StageSlice& slice = result.stages[index];
            StageStats& stage = stats.stages[index];
            stage.responses_ok += slice.responses_ok;
            stage.responses_rejected += slice.responses_rejected;
            stage.responses_refused += slice.responses_refused;
            stage.latencies_us.insert( stage.latencies_us.end(), slice.latencies_us.begin(),
                                       slice.latencies_us.end() );
            stage.ok_latencies_us.insert( stage.ok_latencies_us.end(),
                                          slice.ok_latencies_us.begin(),
                                          slice.ok_latencies_us.end() );
        }
    }
    for ( const StageStats& stage : stats.stages )
    {
        stats.latencies_us.insert( stats.latencies_us.end(), stage.latencies_us.begin(),
                                   stage.latencies_us.end() );
    }
    stats.peak_live_connections = peak.load();
    stats.watchdog_fired = watchdog_fired.load();
    return stats;
}

UInt32 Percentile( const std::vector< UInt32 >& sorted_samples, atlas::Float64 fraction )
{
    if ( sorted_samples.empty() )
    {
        return 0;
    }
    // nearest rank - 요청 분위 이상인 가장 작은 표본
    // 보간하면 아무도 측정하지 않은 값을 p99 로 지어내게 된다
    const auto count = static_cast< atlas::Float64 >( sorted_samples.size() );
    const atlas::Float64 exact_rank = std::ceil( fraction * count );
    std::size_t rank = 1;
    if ( exact_rank > 1.0 )
    {
        rank = static_cast< std::size_t >( exact_rank );
    }
    if ( rank > sorted_samples.size() )
    {
        rank = sorted_samples.size();
    }
    return sorted_samples[rank - 1];
}

}  // namespace atlas_loadgen
