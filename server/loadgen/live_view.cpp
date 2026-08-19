// =============================================================================
// LiveMetrics 집계와 샘플러 스레드 구현. JSONL 기록과 ANSI 화면 갱신
// =============================================================================

#include "loadgen/live_view.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atlas/core/ctx.h"
#include "atlas/core/error.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"

#if defined( _WIN32 )
// [CS 7.2] 아래 escape 는 핸들에 virtual-terminal 처리를 켜야 동작.
// NOMINMAX + WIN32_LEAN_AND_MEAN 은 atlas_core 가 PUBLIC 으로 전파
#include <windows.h>
#endif

namespace atlas_loadgen
{

namespace
{

using atlas::Float64;
using atlas::UInt32;
using atlas::UInt64;

// 샘플러 주기. 화면을 더 빨리 갱신하려면 타이머가 하나 더 필요하다
// 타이머가 둘이면 측정으로 흘러들 수 있는 것도 둘이 된다
constexpr atlas::Millis kSampleInterval{ 1000 };

// 스파크라인 기하. ASCII 고정 - 측정 실행이 콘솔 코드 페이지부터 맞출 일은 없어야 함
constexpr std::string_view kSparkRamp = " .:-=+*#";
constexpr std::size_t kSparkWidth = 48;

// 미측정은 0 이 아니라 null. 0 ms 는 "디스크가 즉답" 으로 읽혀 정반대 진술이 됨
std::string OptionalNumber( Float64 value )
{
    if ( value < 0.0 )
    {
        return "null";
    }
    return std::format( "{:.3f}", value );
}

// 표본이 속한 단계 = 이미 열린 마지막 마크. 항목이 몇 개뿐이라 선형 탐색
std::size_t StageIndexAt( const std::vector< StageMark >& stages, Float64 elapsed_seconds )
{
    std::size_t index = 0;
    for ( std::size_t candidate = 0; candidate < stages.size(); ++candidate )
    {
        if ( stages[candidate].begins_at_seconds <= elapsed_seconds )
        {
            index = candidate;
        }
    }
    return index;
}

}  // namespace

LiveMetrics::LiveMetrics()
{
    for ( std::atomic< UInt32 >& bucket : buckets_ )
    {
        bucket.store( 0, std::memory_order_relaxed );
    }
    for ( std::atomic< UInt32 >& bucket : ok_buckets_ )
    {
        bucket.store( 0, std::memory_order_relaxed );
    }
}

void LiveMetrics::RecordLatency( UInt32 micros, bool accepted ) noexcept
{
    auto index = static_cast< std::size_t >( micros / kBucketMicros );
    if ( index > kRangeBuckets )
    {
        index = kRangeBuckets;
    }
    buckets_[index].fetch_add( 1, std::memory_order_relaxed );
    if ( accepted )
    {
        ok_buckets_[index].fetch_add( 1, std::memory_order_relaxed );
    }
}

void LiveMetrics::RecordError() noexcept { errors_.fetch_add( 1, std::memory_order_relaxed ); }

void LiveMetrics::RecordRejection() noexcept
{
    rejections_.fetch_add( 1, std::memory_order_relaxed );
}

void LiveMetrics::RecordLoadRejection() noexcept
{
    load_rejections_.fetch_add( 1, std::memory_order_relaxed );
}

UInt64 LiveMetrics::DrainInto( Histogram& buckets ) noexcept
{
    UInt64 total = 0;
    for ( std::size_t index = 0; index < kBucketCount; ++index )
    {
        const UInt32 count = buckets[index].exchange( 0, std::memory_order_relaxed );
        scratch_[index] = count;
        total += count;
    }
    return total;
}

void LiveMetrics::Drain( Float64 window_seconds, LiveSample& sample ) noexcept
{
    const UInt64 accepted = DrainInto( ok_buckets_ );
    sample.ok_p50_ms = PercentileMs( accepted, 0.50 );
    sample.ok_p99_ms = PercentileMs( accepted, 0.99 );

    const UInt64 total = DrainInto( buckets_ );
    sample.requests_per_second =
        window_seconds > 0.0 ? static_cast< Float64 >( total ) / window_seconds : 0.0;
    sample.p50_ms = PercentileMs( total, 0.50 );
    sample.p99_ms = PercentileMs( total, 0.99 );
    sample.errors = errors_.load( std::memory_order_relaxed );
    sample.responses = total;
    sample.rejections = rejections_.exchange( 0, std::memory_order_relaxed );
    sample.load_rejections = load_rejections_.load( std::memory_order_relaxed );
}

Float64 LiveMetrics::PercentileMs( UInt64 total, Float64 fraction ) const noexcept
{
    if ( total == 0 )
    {
        return 0.0;
    }
    // load_client.cpp 의 Percentile() 과 같은 nearest rank, 보간 없음.
    // 격자만 다름 - 100us 버킷이라 결과는 상단 경계이고 최대 1버킷 높음
    const Float64 exact_rank = std::ceil( fraction * static_cast< Float64 >( total ) );
    UInt64 rank = exact_rank > 1.0 ? static_cast< UInt64 >( exact_rank ) : UInt64{ 1 };
    if ( rank > total )
    {
        rank = total;
    }

    UInt64 seen = 0;
    for ( std::size_t index = 0; index < kBucketCount; ++index )
    {
        seen += scratch_[index];
        if ( seen >= rank )
        {
            const auto upper_us = static_cast< Float64 >(
                ( index + 1 ) * static_cast< std::size_t >( kBucketMicros ) );
            return upper_us / 1000.0;
        }
    }
    return 0.0;
}

LiveView::LiveView( LiveViewOptions options, LiveMetrics& metrics,
                    const std::atomic< std::size_t >& live )
    : options_( std::move( options ) ), metrics_( metrics ), live_( live )
{
}

LiveView::~LiveView() { Stop(); }

void LiveView::Start()
{
    if ( started_ )
    {
        return;
    }
    started_ = true;

    if ( options_.tui )
    {
#if defined( _WIN32 )
        void* const console = GetStdHandle( STD_OUTPUT_HANDLE );
        DWORD console_mode = 0;
        if ( console != INVALID_HANDLE_VALUE && GetConsoleMode( console, &console_mode ) != 0 )
        {
            SetConsoleMode( console, console_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING );
        }
#endif
        std::fputs( "\x1b[2J\x1b[H\x1b[?25l", stdout );
        std::fflush( stdout );
    }

    // [AD 16.1a] asio 타이머가 아니라 전용 스레드
    // 타이머면 I/O 워커가 처리하게 된다
    // 그러면 렌더러가 자기가 보고하는 요청과 경쟁한다
    worker_ = std::thread( atlas::Guarded( atlas::Ctx{}, [this] { Run(); } ) );
}

void LiveView::Stop()
{
    if ( !started_ )
    {
        return;
    }
    {
        const std::lock_guard< std::mutex > lock( mutex_ );
        stopping_ = true;
    }
    wake_.notify_all();
    if ( worker_.joinable() )
    {
        worker_.join();
    }
    started_ = false;

    if ( options_.tui )
    {
        std::fputs( "\x1b[?25h\n", stdout );
        std::fflush( stdout );
    }
}

void LiveView::Run()
{
    std::ofstream jsonl;
    if ( !options_.jsonl_path.empty() )
    {
        jsonl.open( options_.jsonl_path, std::ios::out | std::ios::trunc );
        ATLAS_CHECK( jsonl.is_open(), "cannot write '{}'", options_.jsonl_path );
        jsonl << options_.meta_json << '\n';
        jsonl.flush();
    }

    const atlas::TimePoint start = atlas::Clock::now();
    atlas::TimePoint due = start + kSampleInterval;
    atlas::TimePoint previous = start;

    while ( true )
    {
        {
            std::unique_lock< std::mutex > lock( mutex_ );
            if ( wake_.wait_until( lock, due, [this] { return stopping_; } ) )
            {
                break;
            }
        }

        const atlas::TimePoint now = atlas::Clock::now();
        const Float64 window_seconds =
            static_cast< Float64 >(
                std::chrono::duration_cast< atlas::Millis >( now - previous ).count() ) /
            1000.0;

        LiveSample sample;
        sample.elapsed_seconds =
            static_cast< Float64 >(
                std::chrono::duration_cast< atlas::Millis >( now - start ).count() ) /
            1000.0;
        sample.inflight = live_.load();
        sample.fsync_probe_ms = ReadProbe();
        metrics_.Drain( window_seconds, sample );
        if ( !options_.stages.empty() )
        {
            sample.stage = StageIndexAt( options_.stages, sample.elapsed_seconds );
            sample.stage_connections = options_.stages[sample.stage].connections;
        }

        if ( jsonl.is_open() )
        {
            // 레코드마다 flush. 최악의 순간에 죽은 실행이야말로 표본이 필요한 실행
            jsonl << std::format( R"({{"kind":"sample","t":{:.3f},"rps":{:.2f},"p50":{:.3f},)"
                                  R"("p99":{:.3f},"ok_p50":{:.3f},"ok_p99":{:.3f},"inflight":{},)"
                                  R"("errors":{},"responses":{},"rejections":{},)"
                                  R"("load_rejections":{},"stage":{},"stage_connections":{},)"
                                  R"("fsync_probe_ms":{}}})",
                                  sample.elapsed_seconds, sample.requests_per_second, sample.p50_ms,
                                  sample.p99_ms, sample.ok_p50_ms, sample.ok_p99_ms,
                                  sample.inflight, sample.errors, sample.responses,
                                  sample.rejections, sample.load_rejections, sample.stage,
                                  sample.stage_connections,
                                  OptionalNumber( sample.fsync_probe_ms ) )
                  << '\n';
            jsonl.flush();
        }
        if ( options_.tui )
        {
            Render( sample );
        }

        previous = now;
        due += kSampleInterval;
        // 뒤처진 샘플러는 짧은 창을 몰아치지 않고 현재 시각으로 따라잡음
        if ( due < now )
        {
            due = now + kSampleInterval;
        }
    }
}

void LiveView::Render( const LiveSample& sample )
{
    spark_.push_back( sample.requests_per_second );
    if ( spark_.size() > kSparkWidth )
    {
        spark_.erase( spark_.begin() );
    }
    Float64 peak = 0.0;
    for ( const Float64 value : spark_ )
    {
        peak = value > peak ? value : peak;
    }

    frame_.clear();
    frame_ += "\x1b[H";
    frame_ += std::format( "atlas_loadgen live   t = {:.0f} s\x1b[K\n", sample.elapsed_seconds );
    frame_ += std::format( "  req/s     {:>10.1f}\x1b[K\n", sample.requests_per_second );
    frame_ += std::format( "  p50       {:>10.1f} ms  (accepted {:.1f})\x1b[K\n", sample.p50_ms,
                           sample.ok_p50_ms );
    frame_ += std::format( "  p99       {:>10.1f} ms  (accepted {:.1f})\x1b[K\n", sample.p99_ms,
                           sample.ok_p99_ms );
    frame_ += std::format( "  inflight  {:>10}\x1b[K\n", sample.inflight );
    frame_ += std::format( "  errors    {:>10}\x1b[K\n", sample.errors );
    // [AD 10.8] errors 아래 별도 줄이며 절대 합산하지 않는다
    // 램프가 용량을 넘을 때 이 줄만 오르고 errors 는 0 이다
    // 그 대비가 곧 관측 결과다
    const Float64 reject_percent = sample.responses > 0
                                       ? ( static_cast< Float64 >( sample.rejections ) * 100.0 ) /
                                             static_cast< Float64 >( sample.responses )
                                       : 0.0;
    frame_ +=
        std::format( "  rejected  {:>10} ({:.1f} %)\x1b[K\n", sample.rejections, reject_percent );
    if ( sample.stage_connections > 0 )
    {
        frame_ += std::format( "  stage     {:>10} ({} connections)\x1b[K\n", sample.stage,
                               sample.stage_connections );
    }
    frame_ += std::format( "  req/s over the last {} s\x1b[K\n", spark_.size() );
    frame_ += "  [";
    for ( const Float64 value : spark_ )
    {
        std::size_t level = 0;
        if ( peak > 0.0 )
        {
            const Float64 scaled =
                ( value / peak ) * static_cast< Float64 >( kSparkRamp.size() - 1 );
            level = static_cast< std::size_t >( std::lround( scaled ) );
        }
        if ( level >= kSparkRamp.size() )
        {
            level = kSparkRamp.size() - 1;
        }
        frame_ += kSparkRamp[level];
    }
    frame_ += std::format( "]  peak {:.0f}\x1b[K\n", peak );

    std::fwrite( frame_.data(), 1, frame_.size(), stdout );
    std::fflush( stdout );
}

Float64 LiveView::ReadProbe() const
{
    if ( options_.probe_file_path.empty() )
    {
        return -1.0;
    }
    std::ifstream probe( options_.probe_file_path );
    if ( !probe.is_open() )
    {
        return -1.0;
    }
    Float64 milliseconds = 0.0;
    if ( !( probe >> milliseconds ) || milliseconds < 0.0 )
    {
        return -1.0;
    }
    return milliseconds;
}

}  // namespace atlas_loadgen
