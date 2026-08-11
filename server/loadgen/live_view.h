#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "atlas/core/types.h"

// The load harness's visualisation axis (architecture-design.md §16.1a). Two layers, split by who
// reads them: the ANSI screen is for watching a run happen, the JSONL is for the report tool
// (tools/loadreport) that turns a finished run into a self-contained HTML page.
//
// 🔴 NOTHING HERE MAY LAND ON THE MEASUREMENT PATH. §16.1a already records one harness that
// measured itself (phase-aligned pacing reported the client's own thundering herd as 53 ms of
// server latency). A renderer sharing the I/O threads would be the same mistake with a different
// name, so the sampler owns a THREAD OF ITS OWN and never runs on an io_context worker. What the
// connections do is one relaxed atomic increment behind a null check, and only when a flag asked
// for it — with neither flag the pointer is null and the default run is byte-for-byte the run
// §16.1 was measured with.
//
// 🔴 THE PERCENTILES BELOW ARE NOT THE RUN'S PERCENTILES. This histogram is 100 µs coarse and is
// drained every second so the live view can move; the figures a run reports still come from the
// exact unsorted sample vector in LoadStats (main.cpp Report). Two ways of computing a p99 must
// never quietly become one, and the cheap one is never the one that gets published.

namespace atlas_loadgen {

// One drained second. Written as one JSONL record and as one screen refresh.
struct LiveSample {
    atlas::Float64 elapsed_seconds{0.0};
    atlas::Float64 requests_per_second{0.0};
    atlas::Float64 p50_ms{0.0};
    atlas::Float64 p99_ms{0.0};
    std::size_t inflight{0};
    atlas::UInt64 errors{0};
    // 🔴 Negative means "no probe was supplied", which is a different statement from 0 ms and is
    // written to the JSONL as null. See LiveViewOptions::probe_file_path for why the harness does
    // not measure this itself.
    atlas::Float64 fsync_probe_ms{-1.0};
};

// The counters the connections write and the sampler thread drains. Every write is relaxed: the
// only reader is the sampler, it wants a snapshot rather than a serialisation point, and ordering
// these would put a memory fence on the response path of every request.
class LiveMetrics {
public:
    LiveMetrics();

    // Called from a connection strand, once per completed round trip.
    void RecordLatency(atlas::UInt32 micros) noexcept;
    // A connect / transport / load failure, or a response the server refused. Cumulative.
    void RecordError() noexcept;

    // Drains the histogram into `sample` and stamps the cumulative error count. 🔴 Sampler thread
    // only — it holds the non-atomic scratch buffer the percentile walk needs.
    void Drain(atlas::Float64 window_seconds, LiveSample& sample) noexcept;

private:
    [[nodiscard]] atlas::Float64 PercentileMs(atlas::UInt64 total,
                                              atlas::Float64 fraction) const noexcept;

    // 100 µs buckets up to 4096 ms, plus one overflow bucket. Coarse on purpose: it exists to draw
    // a curve, and a finer grid would cost more to drain every second than the picture is worth.
    static constexpr atlas::UInt32 kBucketMicros = 100;
    static constexpr std::size_t kRangeBuckets = 40960;
    static constexpr std::size_t kBucketCount = kRangeBuckets + 1;

    std::array<std::atomic<atlas::UInt32>, kBucketCount> buckets_;
    std::array<atlas::UInt32, kBucketCount> scratch_{};
    std::atomic<atlas::UInt64> errors_{0};
};

struct LiveViewOptions {
    // The ANSI screen. Off by default; it is a demo aid, not part of a measurement.
    bool tui{false};
    // Empty = write no JSONL.
    std::string jsonl_path;
    // 🔴 A file an EXTERNAL probe loop keeps up to date, not something this process measures. The
    // regime discriminator of §16.1c-① is an O_DSYNC probe against the MySQL container's data
    // volume; a client-side probe would time a different disk path (the Windows side of the vhdx)
    // and, worse, would add synchronous writes to the host while the host is the thing under test.
    // So the harness only transcribes: the file holds the latest probe in milliseconds, the
    // sampler reads it once a second, and an empty path leaves the field null.
    std::string probe_file_path;
    // Written verbatim as the JSONL's first record. Built by the caller because it is the caller
    // that knows the run's options; the conditions a run cannot know about itself (host, container,
    // MySQL settings) stay in §16.1b and are passed to the report tool as notes.
    std::string meta_json;
};

// The sampler. Owns one thread, wakes once a second, drains LiveMetrics, and writes whatever the
// options asked for.
class LiveView {
public:
    LiveView(LiveViewOptions options, LiveMetrics& metrics, const std::atomic<std::size_t>& live);

    LiveView(const LiveView&) = delete;
    LiveView& operator=(const LiveView&) = delete;
    LiveView(LiveView&&) = delete;
    LiveView& operator=(LiveView&&) = delete;
    ~LiveView();

    void Start();
    void Stop();

private:
    void Run();
    void Render(const LiveSample& sample);
    [[nodiscard]] atlas::Float64 ReadProbe() const;

    LiveViewOptions options_;
    LiveMetrics& metrics_;
    const std::atomic<std::size_t>& live_;

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_{false};
    bool started_{false};

    // Throughput history for the sparkline, newest last, capped at the strip width.
    std::vector<atlas::Float64> spark_;
    std::string frame_;
};

}  // namespace atlas_loadgen
