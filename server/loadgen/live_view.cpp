#include "loadgen/live_view.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include "atlas/core/ctx.h"
#include "atlas/core/error.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"

#if defined(_WIN32)
// 🔴 The escape sequences below are inert on a stock conhost unless virtual-terminal processing is
// turned on for the handle. Windows.h arrives with NOMINMAX + WIN32_LEAN_AND_MEAN already defined:
// atlas_core propagates both PUBLIC (cpp-style.md §7.2), which is exactly the unity-build
// contamination this file would otherwise cause.
#include <windows.h>
#endif

namespace atlas_loadgen {

namespace {

using atlas::Float64;
using atlas::UInt32;
using atlas::UInt64;

// How often the sampler wakes. One second is the JSONL's record rate and the screen's refresh rate
// at once: a faster screen would need a second timer, and a second timer is a second thing that can
// drift into the measurement.
constexpr atlas::Millis kSampleInterval{1000};

// Sparkline geometry. ASCII on purpose — the block-drawing glyphs need a UTF-8 console code page
// and the harness's own terminal is not something a measurement should have to configure.
constexpr std::string_view kSparkRamp = " .:-=+*#";
constexpr std::size_t kSparkWidth = 48;

// One JSON number, or `null` when the field was never measured. 🔴 An absent probe must not print
// as 0 — 0 ms reads as "the disk answered instantly", which is the opposite of "nobody looked".
std::string OptionalNumber(Float64 value) {
    if (value < 0.0) {
        return "null";
    }
    return std::format("{:.3f}", value);
}

}  // namespace

LiveMetrics::LiveMetrics() {
    for (std::atomic<UInt32>& bucket : buckets_) {
        bucket.store(0, std::memory_order_relaxed);
    }
}

void LiveMetrics::RecordLatency(UInt32 micros) noexcept {
    auto index = static_cast<std::size_t>(micros / kBucketMicros);
    if (index > kRangeBuckets) {
        index = kRangeBuckets;
    }
    buckets_[index].fetch_add(1, std::memory_order_relaxed);
}

void LiveMetrics::RecordError() noexcept { errors_.fetch_add(1, std::memory_order_relaxed); }

void LiveMetrics::Drain(Float64 window_seconds, LiveSample& sample) noexcept {
    UInt64 total = 0;
    for (std::size_t index = 0; index < kBucketCount; ++index) {
        const UInt32 count = buckets_[index].exchange(0, std::memory_order_relaxed);
        scratch_[index] = count;
        total += count;
    }

    sample.requests_per_second =
        window_seconds > 0.0 ? static_cast<Float64>(total) / window_seconds : 0.0;
    sample.p50_ms = PercentileMs(total, 0.50);
    sample.p99_ms = PercentileMs(total, 0.99);
    sample.errors = errors_.load(std::memory_order_relaxed);
}

Float64 LiveMetrics::PercentileMs(UInt64 total, Float64 fraction) const noexcept {
    if (total == 0) {
        return 0.0;
    }
    // Nearest rank, same convention as Percentile() in load_client.cpp — no interpolation. The
    // difference is the grid: this walks 100 µs buckets, so the answer is the bucket's upper edge
    // and is high by at most one bucket.
    const Float64 exact_rank = std::ceil(fraction * static_cast<Float64>(total));
    UInt64 rank = exact_rank > 1.0 ? static_cast<UInt64>(exact_rank) : UInt64{1};
    if (rank > total) {
        rank = total;
    }

    UInt64 seen = 0;
    for (std::size_t index = 0; index < kBucketCount; ++index) {
        seen += scratch_[index];
        if (seen >= rank) {
            const auto upper_us =
                static_cast<Float64>((index + 1) * static_cast<std::size_t>(kBucketMicros));
            return upper_us / 1000.0;
        }
    }
    return 0.0;
}

LiveView::LiveView(LiveViewOptions options, LiveMetrics& metrics,
                   const std::atomic<std::size_t>& live)
    : options_(std::move(options)), metrics_(metrics), live_(live) {}

LiveView::~LiveView() { Stop(); }

void LiveView::Start() {
    if (started_) {
        return;
    }
    started_ = true;

    if (options_.tui) {
#if defined(_WIN32)
        void* const console = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD console_mode = 0;
        if (console != INVALID_HANDLE_VALUE && GetConsoleMode(console, &console_mode) != 0) {
            SetConsoleMode(console, console_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
#endif
        // Clear, home, hide the cursor. Every refresh after this one repaints in place.
        std::fputs("\x1b[2J\x1b[H\x1b[?25l", stdout);
        std::fflush(stdout);
    }

    // 🔴 A thread of its own, not an asio timer on the harness's io_context: a timer there would be
    // dispatched by an I/O worker and the renderer would then be competing with the requests it is
    // reporting on (§16.1a).
    worker_ = std::thread(atlas::Guarded(atlas::Ctx{}, [this] { Run(); }));
}

void LiveView::Stop() {
    if (!started_) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    started_ = false;

    if (options_.tui) {
        std::fputs("\x1b[?25h\n", stdout);
        std::fflush(stdout);
    }
}

void LiveView::Run() {
    std::ofstream jsonl;
    if (!options_.jsonl_path.empty()) {
        jsonl.open(options_.jsonl_path, std::ios::out | std::ios::trunc);
        ATLAS_CHECK(jsonl.is_open(), "cannot write '{}'", options_.jsonl_path);
        jsonl << options_.meta_json << '\n';
        jsonl.flush();
    }

    const atlas::TimePoint start = atlas::Clock::now();
    atlas::TimePoint due = start + kSampleInterval;
    atlas::TimePoint previous = start;

    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (wake_.wait_until(lock, due, [this] { return stopping_; })) {
                break;
            }
        }

        const atlas::TimePoint now = atlas::Clock::now();
        const Float64 window_seconds =
            static_cast<Float64>(
                std::chrono::duration_cast<atlas::Millis>(now - previous).count()) /
            1000.0;

        LiveSample sample;
        sample.elapsed_seconds =
            static_cast<Float64>(std::chrono::duration_cast<atlas::Millis>(now - start).count()) /
            1000.0;
        sample.inflight = live_.load();
        sample.fsync_probe_ms = ReadProbe();
        metrics_.Drain(window_seconds, sample);

        if (jsonl.is_open()) {
            // 🔴 Flushed per record. A run that is killed at its worst moment is exactly the run
            // whose samples are worth having, and a buffered tail would lose it.
            jsonl << std::format(R"({{"kind":"sample","t":{:.3f},"rps":{:.2f},"p50":{:.3f},)"
                                 R"("p99":{:.3f},"inflight":{},"errors":{},"fsync_probe_ms":{}}})",
                                 sample.elapsed_seconds, sample.requests_per_second, sample.p50_ms,
                                 sample.p99_ms, sample.inflight, sample.errors,
                                 OptionalNumber(sample.fsync_probe_ms))
                  << '\n';
            jsonl.flush();
        }
        if (options_.tui) {
            Render(sample);
        }

        previous = now;
        due += kSampleInterval;
        // A sampler that fell behind catches up to now rather than firing a burst of short windows.
        if (due < now) {
            due = now + kSampleInterval;
        }
    }
}

void LiveView::Render(const LiveSample& sample) {
    spark_.push_back(sample.requests_per_second);
    if (spark_.size() > kSparkWidth) {
        spark_.erase(spark_.begin());
    }
    Float64 peak = 0.0;
    for (const Float64 value : spark_) {
        peak = value > peak ? value : peak;
    }

    frame_.clear();
    frame_ += "\x1b[H";
    // ASCII only, for the same reason the sparkline is: the console code page is not something a
    // measurement run should have to configure first.
    frame_ += std::format("atlas_loadgen live   t = {:.0f} s\x1b[K\n", sample.elapsed_seconds);
    frame_ += std::format("  req/s     {:>10.1f}\x1b[K\n", sample.requests_per_second);
    frame_ += std::format("  p50       {:>10.1f} ms\x1b[K\n", sample.p50_ms);
    frame_ += std::format("  p99       {:>10.1f} ms\x1b[K\n", sample.p99_ms);
    frame_ += std::format("  inflight  {:>10}\x1b[K\n", sample.inflight);
    frame_ += std::format("  errors    {:>10}\x1b[K\n", sample.errors);
    frame_ += std::format("  req/s over the last {} s\x1b[K\n", spark_.size());
    frame_ += "  [";
    for (const Float64 value : spark_) {
        std::size_t level = 0;
        if (peak > 0.0) {
            const Float64 scaled = (value / peak) * static_cast<Float64>(kSparkRamp.size() - 1);
            level = static_cast<std::size_t>(std::lround(scaled));
        }
        if (level >= kSparkRamp.size()) {
            level = kSparkRamp.size() - 1;
        }
        frame_ += kSparkRamp[level];
    }
    frame_ += std::format("]  peak {:.0f}\x1b[K\n", peak);

    std::fwrite(frame_.data(), 1, frame_.size(), stdout);
    std::fflush(stdout);
}

Float64 LiveView::ReadProbe() const {
    if (options_.probe_file_path.empty()) {
        return -1.0;
    }
    std::ifstream probe(options_.probe_file_path);
    if (!probe.is_open()) {
        return -1.0;
    }
    Float64 milliseconds = 0.0;
    if (!(probe >> milliseconds) || milliseconds < 0.0) {
        return -1.0;
    }
    return milliseconds;
}

}  // namespace atlas_loadgen
