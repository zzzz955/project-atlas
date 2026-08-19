#pragma once

// =============================================================================
// 부하 하네스의 관측 축. ANSI 화면은 실행을 보기 위한 것
// JSONL 은 리포트 도구(tools/loadreport)의 입력
// =============================================================================

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "atlas/core/types.h"

// [AD 16.1a] 어떤 것도 측정 경로에 올라가면 안 된다
// 샘플러는 전용 스레드를 소유하고 io_context 워커에서 돌지 않는다
// 연결은 null 검사 뒤 relaxed 증가 1회뿐이라 기본 실행은 기존 측정과 동일하다
// 여기 백분위는 실행의 백분위가 아니라 100us 격자에 매초 비워지는 값이다
// 보고 값은 LoadStats 의 표본 벡터에서 나온다. 싼 쪽이 발표되면 안 된다

namespace atlas_loadgen
{

// 1초를 비워 낸 결과. JSONL 1레코드 = 화면 1갱신
struct LiveSample
{
    atlas::Float64 elapsed_seconds{ 0.0 };
    atlas::Float64 requests_per_second{ 0.0 };
    atlas::Float64 p50_ms{ 0.0 };
    atlas::Float64 p99_ms{ 0.0 };
    // 수락된 응답만 본 같은 두 값
    // 서버가 실제로 일한 시간은 이쪽이다
    // 위 두 값은 거부까지 섞인 클라이언트 관점
    atlas::Float64 ok_p50_ms{ 0.0 };
    atlas::Float64 ok_p99_ms{ 0.0 };
    std::size_t inflight{ 0 };
    atlas::UInt64 errors{ 0 };
    // [AD 10.8] 이 창의 완료 왕복 수와 그중 거부 수
    // 분모 없는 거부 수는 비율로 읽을 수 없다
    // 셰딩인지 사망인지는 비율만이 말해 준다
    atlas::UInt64 responses{ 0 };
    atlas::UInt64 rejections{ 0 };
    // 누적값. 요청당이 아니라 연결당 1회
    // 창 단위로 세면 대부분 0 이 되어 "거부 없음" 으로 읽힌다
    atlas::UInt64 load_rejections{ 0 };
    // 이 창이 속한 램프 단계와 그 단계의 연결 수. 고정 동시성 실행이면 0
    std::size_t stage{ 0 };
    std::size_t stage_connections{ 0 };
    // 음수 = 프로브 미제공. 0 ms 와 다른 진술이며 JSONL 에는 null 로 기록.
    // 하네스가 직접 재지 않는 이유는 probe_file_path 참고
    atlas::Float64 fsync_probe_ms{ -1.0 };
};

// 연결이 쓰고 샘플러가 비우는 카운터. 모든 쓰기는 relaxed
// 스냅샷이면 충분하다
// 순서를 주면 매 요청 응답 경로에 메모리 펜스가 박힌다
class LiveMetrics
{
public:
    LiveMetrics();

    // 연결 strand 에서 왕복 완료마다 호출. accepted 로 히스토그램을 둘로 나눈다
    // 거부는 us 단위로 답해, 다수가 되면 섞인 p50 이 무너져 오독을 부른다
    void RecordLatency( atlas::UInt32 micros, bool accepted ) noexcept;
    // connect/전송/load 실패 또는 서버가 거절한 응답. 누적
    void RecordError() noexcept;
    // [AD 10.8] 에러가 아님. 서버가 부하 중 작업을 사절하고 연결은 유지한 것.
    // "거절했다" 와 "망가졌다" 는 반대 판독이라 계열을 나눔
    void RecordRejection() noexcept;
    void RecordLoadRejection() noexcept;

    // 히스토그램을 sample 로 비움. 비원자 scratch 를 쓰므로 샘플러 스레드 전용
    void Drain( atlas::Float64 window_seconds, LiveSample& sample ) noexcept;

private:
    // 4096 ms 까지 100us 버킷 + 오버플로 1개
    // 곡선을 그리는 용도라 의도적으로 성기다
    // 더 촘촘하면 매초 비우는 비용이 그림값을 넘는다
    static constexpr atlas::UInt32 kBucketMicros = 100;
    static constexpr std::size_t kRangeBuckets = 40960;
    static constexpr std::size_t kBucketCount = kRangeBuckets + 1;

    using Histogram = std::array< std::atomic< atlas::UInt32 >, kBucketCount >;

    [[nodiscard]] atlas::Float64 PercentileMs( atlas::UInt64 total,
                                               atlas::Float64 fraction ) const noexcept;
    atlas::UInt64 DrainInto( Histogram& buckets ) noexcept;

    Histogram buckets_;
    Histogram ok_buckets_;
    std::array< atlas::UInt32, kBucketCount > scratch_{};
    std::atomic< atlas::UInt64 > errors_{ 0 };
    // 히스토그램과 같이 창 단위로 비운다
    // 그래야 응답 수와의 비가 누적합이 아니라 비율이 된다
    std::atomic< atlas::UInt64 > rejections_{ 0 };
    std::atomic< atlas::UInt64 > load_rejections_{ 0 };
};

struct StageMark
{
    atlas::Float64 begins_at_seconds{ 0.0 };
    std::size_t connections{ 0 };
};

struct LiveViewOptions
{
    // ANSI 화면. 기본 off - 데모 보조물이지 측정의 일부가 아님
    bool tui{ false };
    // 비면 JSONL 미기록
    std::string jsonl_path;
    // [AD 16.1c] 외부 프로브 루프가 갱신하는 파일이다
    // 이 프로세스의 측정이 아니다
    // 클라이언트 측 프로브는 다른 디스크 경로를 재고 시험 대상에 쓰기를 더한다
    // 여기서는 매초 읽어 옮기기만 한다
    std::string probe_file_path;
    // JSONL 첫 레코드로 그대로 기록한다
    // 실행 옵션을 아는 것은 호출자이므로 호출자가 구성한다
    // 실행이 스스로 알 수 없는 조건은 리포트 도구에 별도 전달한다
    std::string meta_json;
    // 램프 일정, 시작 시각 오름차순. 고정 동시성이면 1개
    std::vector< StageMark > stages;
};

// 샘플러. 전용 스레드 1개로 매초 깨어나 LiveMetrics 를 비우고 기록
class LiveView
{
public:
    LiveView( LiveViewOptions options, LiveMetrics& metrics,
              const std::atomic< std::size_t >& live );

    LiveView( const LiveView& ) = delete;
    LiveView& operator=( const LiveView& ) = delete;
    LiveView( LiveView&& ) = delete;
    LiveView& operator=( LiveView&& ) = delete;
    ~LiveView();

    void Start();
    void Stop();

private:
    void Run();
    void Render( const LiveSample& sample );
    [[nodiscard]] atlas::Float64 ReadProbe() const;

    LiveViewOptions options_;
    LiveMetrics& metrics_;
    const std::atomic< std::size_t >& live_;

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_{ false };
    bool started_{ false };

    // 스파크라인용 처리량 이력, 최신이 뒤. 스트립 폭으로 제한
    std::vector< atlas::Float64 > spark_;
    std::string frame_;
};

}  // namespace atlas_loadgen
