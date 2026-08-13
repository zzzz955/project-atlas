#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "atlas/core/types.h"

// The load harness (architecture-design.md §16 — the row that says the tick/bandwidth targets are
// worked backwards from a first load measurement, and §9 for the model this reuses).
//
// 🔴 ONE CONNECTION IS NOT ONE THREAD. A thread-per-connection client stops measuring the server
// and starts measuring its own scheduler: past a few hundred threads the context switches dominate
// and every number after that describes the client. This harness uses the SAME model the server
// does (§9) — one io_context, a fixed worker pool, one strand per connection — so the client scales
// the way the server does and the limit that shows up is the server's.
//
// 🔴 It reuses the frame layer (atlas/proto) and the demo game's opcode table (game/handlers.h)
// instead of re-encoding the wire. A second framing implementation would carry its own bugs, and
// then a run would be a debugging session rather than a measurement.
//
// 🔴 It does not decide what a "good" number is. It reports what happened — including the failures
// — and the interpretation lives in the design document next to the conditions it was measured
// under. A harness that hid dropped connections would make every other figure unreadable.

namespace atlas_loadgen {

struct LoadOptions {
    // A literal IPv4/IPv6 address, not a hostname: name resolution in the middle of a load run is
    // one more thing that can stall and it would land inside the latency samples.
    std::string host{"127.0.0.1"};
    atlas::UInt16 port{7777};

    std::size_t connections{1};

    // Target aggregate requests per second across every connection. 🔴 0 means CLOSED LOOP — the
    // next request leaves as soon as the previous reply lands. That is the mode that finds a
    // saturation point, because an open loop at a rate the server cannot serve just grows a queue
    // in the client and reports the client's backlog as latency.
    atlas::UInt32 rate_per_second{0};

    // Whole run, warm-up included.
    atlas::UInt32 duration_seconds{20};
    // Discarded prefix. Connection set-up, the first character load and the driver's first
    // prepared-statement round trip all land here and none of them are steady state.
    atlas::UInt32 warmup_seconds{3};

    std::size_t io_threads{4};

    // The seeded character block. Connection i drives character `first_character_id + i`, so no two
    // connections contend on the per-character lock (§10.5) — that lock is a real serialiser and
    // pointing every connection at one character would measure it instead of the server.
    atlas::UInt64 first_character_id{900000};
    atlas::UInt16 server_id{1};

    // `TCP_NODELAY` on the HARNESS's sockets (§16.1i). 🔴 Absent = the option is not touched at
    // all, which is what every §16.1 run did and is why the default is an empty optional rather
    // than `false`.
    //
    // 🔴 THIS IS THE CLIENT'S SOCKET, NOT THE SERVER'S. §9.3-(3) sets `no_delay(true)` on every
    // accepted socket from a literal in `atlas/net/acceptor.cpp`, and there is no runtime key for
    // it — contrasting THAT side means rebuilding the image, which is a change to the core rather
    // than to this harness. What this flag contrasts is the same option, the same mechanism and the
    // same loopback path, on the request half of the round trip.
    std::optional<bool> no_delay;

    // The ramp-up axis (§16.1i). Each entry is a STAGE's connection count, in order, and every
    // stage runs for `stage_seconds`. Connections join at stage boundaries and never leave, so the
    // list has to be strictly increasing.
    //
    // 🔴 EMPTY IS THE DEFAULT AND IT IS THE FIXED-CONCURRENCY MODE EVERY §16.1 TABLE WAS MEASURED
    // WITH. Ramp-up is an added mode, not a replacement: the moment the default changes, the path
    // back to the numbers already in the document is gone and none of them can be re-run.
    std::vector<std::size_t> ramp_stages;
    atlas::UInt32 stage_seconds{0};

    // The visualisation axis (§16.1a · loadgen/live_view.h). 🔴 BOTH DEFAULT TO OFF AND THAT IS
    // LOAD BEARING: with neither set the harness allocates no metrics sink, starts no sampler
    // thread and prints exactly what it printed for the §16.1 tables, so a run taken today is
    // comparable with the numbers already in the document.
    bool tui{false};
    std::string sample_jsonl_path;
    // Path to a file an external probe loop keeps current, in milliseconds. The harness reads it,
    // never measures it — live_view.h says why.
    std::string probe_file_path;
};

// One stage's steady window. 🔴 A fixed-concurrency run is ONE stage, so this is not a second way
// of reporting — it is the same aggregation with the stage list length set to 1.
struct StageStats {
    std::size_t connections{0};
    atlas::UInt32 window_ms{0};
    std::size_t responses_ok{0};
    // 🔴 REJECTED, NOT FAILED. `kEquipResponseUnavailable` past the DB queue cap (§10.8) says the
    // server declined the work and kept the connection; the client's next request is served
    // normally. Adding this to the failure count would turn "the server refused" into "the server
    // broke", and those are opposite conclusions about the same run.
    std::size_t responses_rejected{0};
    std::size_t responses_refused{0};
    std::vector<atlas::UInt32> latencies_us;
    // 🔴 THE ACCEPTED WORK ONLY, and the table is unreadable without it. A rejection comes back in
    // microseconds because nothing ran, so once most answers are rejections the mixed p50 COLLAPSES
    // — and a reader would take that for "the server got faster under overload". The question the
    // ramp is asked is what happens to the requests the server did serve.
    std::vector<atlas::UInt32> ok_latencies_us;
};

struct LoadStats {
    std::size_t connections_attempted{0};
    std::size_t connections_established{0};
    std::size_t connect_failures{0};
    // Established, then lost before the run ended: a reset, a framing error, or a close from the
    // far side. Counted apart from connect failures because they mean different things.
    std::size_t transport_failures{0};
    std::size_t load_failures{0};
    std::size_t peak_live_connections{0};

    std::size_t requests_sent{0};
    std::size_t responses_ok{0};
    // The server ran the job without a connection: the pool was exhausted within its acquire
    // timeout (§10.3). 🔴 This is the hard pool-saturation signal and it is counted separately from
    // every other refusal.
    std::size_t responses_unavailable{0};
    std::size_t responses_refused{0};
    // A character load the server declined the same way (`LoadResult::Unavailable`). 🔴 Counted and
    // RETRIED, never fatal: past the queue cap a joining connection can be turned away on its first
    // request, and treating that as a lost connection would delete concurrency from the ramp
    // exactly at the load level the ramp exists to describe.
    std::size_t loads_rejected{0};
    // Connections whose `--no-delay` request the OS refused. 🔴 Reported, because a non-zero here
    // means the cell measured something other than its own label.
    std::size_t no_delay_failures{0};

    // Steady-state equip round trips, microseconds, unsorted. In a ramp-up run this is the union of
    // the stages' windows and 🔴 the throughput derived from it is an average across stages, not a
    // capacity — `stages` below is what carries the answer.
    std::vector<atlas::UInt32> latencies_us;
    atlas::UInt32 steady_window_ms{0};
    std::vector<StageStats> stages;
    // The watchdog had to stop the io_context because a connection never came back. Any run with
    // this set is reported as suspect rather than quietly trimmed.
    bool watchdog_fired{false};
};

// Runs the whole sweep point and returns once every connection has finished or been stopped.
[[nodiscard]] LoadStats RunLoad(const LoadOptions& options);

// Nearest-rank percentile over an ASCENDING-sorted sample vector. 0 for an empty vector.
// 🔴 `fraction` is 0.50 / 0.99, not 50 / 99 — the two conventions get mixed up exactly once per
// project and the result is a p99 that is really a p50.
[[nodiscard]] atlas::UInt32 Percentile(const std::vector<atlas::UInt32>& sorted_samples,
                                       atlas::Float64 fraction);

}  // namespace atlas_loadgen
