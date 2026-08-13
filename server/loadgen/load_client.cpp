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

namespace atlas_loadgen {

namespace {

using atlas::Byte;
using atlas::Int64;
using atlas::UInt16;
using atlas::UInt32;
using atlas::UInt64;
using atlas::UInt8;

// One socket read. A response here is a few dozen bytes, so this only ever holds a handful of
// frames; the reassembly bound is FrameReader's, not this buffer's.
constexpr std::size_t kReadBufferSize = 4096;

// How long past the run deadline the watchdog waits. A connection that has not come back by then is
// not slow, it is stuck, and the run has to end and say so rather than hang.
constexpr atlas::Seconds kWatchdogGrace{10};

// Every request drives the same three writes across two tables (§10.5): the target slot is always
// occupied, because the two seeded items take turns in it.
constexpr atlas_demo::EquipSlot kDrivenSlot = atlas_demo::EquipSlot::Weapon;

// How long a connection waits before asking for its character again after the server declined the
// load (§10.8). 🔴 Long enough that the retry is not itself part of the burst that was just shed,
// short enough that a joining connection still reaches its stage's steady window.
constexpr atlas::Millis kLoadRetryDelay{250};

// One stage's clock. A fixed-concurrency run has exactly one of these and it spans the whole run,
// so the stage machinery below is not a second code path — it is the same one with a list of 1.
struct StageWindow {
    std::size_t connections{0};
    atlas::TimePoint begin{};
    // Samples before this instant are discarded: connect, the first character load and the driver's
    // first prepare of each statement all land here (§16.1a), and in a ramp they land again every
    // time a stage adds connections.
    atlas::TimePoint steady_begin{};
    atlas::TimePoint end{};
};

// A connection's slice of one stage.
struct StageSlice {
    std::size_t responses_ok{0};
    std::size_t responses_rejected{0};
    std::size_t responses_refused{0};
    std::vector<UInt32> latencies_us;
    std::vector<UInt32> ok_latencies_us;
};

// What one connection observed. Merged into LoadStats after the io_context has drained, so nothing
// here needs a lock — the owning connection touches it only from its own strand.
struct ConnectionResult {
    bool established{false};
    bool connect_failed{false};
    bool transport_failed{false};
    bool load_failed{false};
    std::size_t requests_sent{0};
    std::size_t responses_ok{0};
    std::size_t responses_unavailable{0};
    std::size_t responses_refused{0};
    std::size_t loads_rejected{0};
    // 🔴 The socket option the run asked for did not take. A cell that silently measured the
    // opposite of its label is worse than no cell.
    bool no_delay_failed{false};
    std::vector<StageSlice> stages;
};

// One connection: connect, load the character, then equip in a strict request/response ping-pong
// until the deadline.
//
// 🔴 One strand per connection, exactly as the server does it (§9.1). With a single request in
// flight the chain is already sequential, so the strand is not load bearing for correctness here —
// it is load bearing for what the harness demonstrates: the client is the same concurrency model as
// the server, and neither side is a thread farm.
class LoadConnection : public std::enable_shared_from_this<LoadConnection> {
public:
    LoadConnection(atlas::IoContext& io_context, atlas::Endpoint endpoint, UInt64 character_id,
                   atlas::Duration interval, atlas::TimePoint first_send,
                   atlas::TimePoint activate_at, const std::vector<StageWindow>& windows,
                   std::optional<bool> no_delay, std::atomic<std::size_t>& live,
                   std::atomic<std::size_t>& peak, LiveMetrics* metrics,
                   std::function<void()> on_finished)
        : strand_(atlas::asio::make_strand(io_context)),
          socket_(strand_),
          timer_(strand_),
          endpoint_(std::move(endpoint)),
          character_id_(character_id),
          interval_(interval),
          // ScheduleNext advances by one interval before it waits, so the first paced send lands on
          // `first_send` exactly.
          next_send_(first_send - interval),
          activate_at_(activate_at),
          windows_(windows),
          no_delay_(no_delay),
          deadline_(windows.back().end),
          live_(live),
          peak_(peak),
          metrics_(metrics),
          on_finished_(std::move(on_finished)) {
        result_.stages.resize(windows.size());
    }

    LoadConnection(const LoadConnection&) = delete;
    LoadConnection& operator=(const LoadConnection&) = delete;
    LoadConnection(LoadConnection&&) = delete;
    LoadConnection& operator=(LoadConnection&&) = delete;
    ~LoadConnection() = default;

    // 🔴 A connection that joins a later ramp stage waits here rather than connecting with the
    // others: the stage list is what defines the run's concurrency at each moment, and a client
    // that opened all of them up front would report a ramp it never applied.
    void Start() {
        if (activate_at_ <= atlas::Clock::now()) {
            Connect();
            return;
        }
        auto self = shared_from_this();
        timer_.expires_at(activate_at_);
        timer_.async_wait(atlas::asio::bind_executor(
            strand_, atlas::Guarded(atlas::Ctx{}, [self](const atlas::ErrorCode& error) {
                if (error || self->closed_) {
                    return;
                }
                self->Connect();
            })));
    }

    // Callable from any thread — the watchdog uses it.
    void Stop() {
        auto self = shared_from_this();
        atlas::asio::post(strand_, atlas::Guarded(atlas::Ctx{}, [self] { self->Close(); }));
    }

    [[nodiscard]] const ConnectionResult& Result() const noexcept { return result_; }

private:
    void Connect() {
        auto self = shared_from_this();
        socket_.async_connect(
            endpoint_,
            atlas::asio::bind_executor(
                strand_, atlas::Guarded(atlas::Ctx{}, [self](const atlas::ErrorCode& error) {
                    self->OnConnect(error);
                })));
    }

    void OnConnect(const atlas::ErrorCode& error) {
        if (error) {
            result_.connect_failed = true;
            NoteError();
            Finish();
            return;
        }
        result_.established = true;
        // 🔴 Only when asked. An untouched socket is what §16.1's tables were taken through, and a
        // harness that quietly started setting options would make today's runs incomparable with
        // them while looking like it had changed nothing.
        if (no_delay_.has_value()) {
            // Same explicit-discard idiom as atlas/net/acceptor.cpp: the synchronous overload
            // reports failure through both the out-param and the return value. Not fatal — a
            // connection with Nagle on still works, it is only slower.
            atlas::ErrorCode nodelay_ec;
            std::ignore = socket_.set_option(atlas::Tcp::no_delay(*no_delay_), nodelay_ec);
            if (nodelay_ec) {
                result_.no_delay_failed = true;
            }
        }
        BumpLive();
        ReadMore();
        SendLoadRequest();
    }

    void ReadMore() {
        auto self = shared_from_this();
        socket_.async_read_some(
            atlas::asio::buffer(read_buffer_),
            atlas::asio::bind_executor(
                strand_, atlas::Guarded(atlas::Ctx{}, [self](const atlas::ErrorCode& error,
                                                             std::size_t bytes_read) {
                    self->OnRead(error, bytes_read);
                })));
    }

    void OnRead(const atlas::ErrorCode& error, std::size_t bytes_read) {
        if (closed_) {
            return;
        }
        if (error) {
            OnTransportError();
            return;
        }

        frames_.clear();
        const atlas::FrameError framing =
            reader_.Feed(std::span<const Byte>(read_buffer_.data(), bytes_read), frames_);
        // Frames that decoded before a bad one are still delivered first (frame_reader.h).
        for (const atlas::Frame& frame : frames_) {
            OnFrame(frame);
        }
        if (framing != atlas::FrameError::None) {
            OnTransportError();
            return;
        }
        if (closed_) {
            return;
        }
        ReadMore();
    }

    void OnFrame(const atlas::Frame& frame) {
        if (closed_) {
            return;
        }
        if (frame.opcode == atlas_demo::kOpCharacterLoadResponse) {
            OnLoadResponse(frame);
            return;
        }
        if (frame.opcode == atlas_demo::kOpEquipResponse) {
            OnEquipResponse(frame);
            return;
        }
        // An opcode this harness did not ask for means the two sides disagree about the protocol.
        // Same verdict the server gives (handlers.cpp): stop, do not guess.
        OnTransportError();
    }

    void OnLoadResponse(const atlas::Frame& frame) {
        std::span<const Byte> cursor(frame.payload);
        UInt8 result = 0xFF;
        if (!atlas::generated::ReadLe(cursor, result)) {
            result_.load_failed = true;
            NoteError();
            Close();
            return;
        }
        // 🔴 A DECLINED LOAD IS NOT A LOST CONNECTION. Past the DB queue cap the server answers
        // LoadResult::Unavailable and keeps the socket open (§10.8), so the honest response is to
        // ask again. Closing here would remove the connection from the ramp at exactly the load
        // level the ramp exists to describe, and the table would then show the client backing off
        // while claiming to show the server shedding.
        if (result == static_cast<UInt8>(atlas_demo::LoadResult::Unavailable)) {
            ++result_.loads_rejected;
            NoteLoadRejection();
            RetryLoadLater();
            return;
        }
        if (result != static_cast<UInt8>(atlas_demo::LoadResult::Ok)) {
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
        const bool decoded =
            atlas::generated::ReadLe(cursor, loaded_character) &&
            atlas::generated::ReadLe(cursor, account_uid) &&
            atlas::generated::ReadUtf8(cursor, name) && atlas::generated::ReadLe(cursor, pos_x) &&
            atlas::generated::ReadLe(cursor, pos_y) && atlas::generated::ReadLe(cursor, level) &&
            atlas::generated::ReadLe(cursor, exp) && atlas::generated::ReadLe(cursor, item_count);
        if (!decoded) {
            result_.load_failed = true;
            NoteError();
            Close();
            return;
        }

        std::size_t taken = 0;
        for (UInt16 index = 0; index < item_count; ++index) {
            UInt64 item_uid = 0;
            UInt32 item_id = 0;
            UInt16 stack_count = 0;
            UInt8 slot = 0;
            if (!atlas::generated::ReadLe(cursor, item_uid) ||
                !atlas::generated::ReadLe(cursor, item_id) ||
                !atlas::generated::ReadLe(cursor, stack_count) ||
                !atlas::generated::ReadLe(cursor, slot)) {
                break;
            }
            if (taken < item_uids_.size()) {
                item_uids_[taken] = item_uid;
                ++taken;
            }
        }

        // 🔴 Two items are the scenario, not a convenience: they take turns in one slot, so every
        // request unequips a real occupant and all three writes of §10.5 run. One item would leave
        // write 1 idle and the measurement would describe a cheaper transaction than the server's.
        if (taken < item_uids_.size()) {
            result_.load_failed = true;
            NoteError();
            Close();
            return;
        }

        ScheduleNext();
    }

    void OnEquipResponse(const atlas::Frame& frame) {
        const atlas::TimePoint arrived = atlas::Clock::now();

        std::span<const Byte> cursor(frame.payload);
        UInt8 code = atlas_demo::kEquipResponseUnavailable;
        if (!atlas::generated::ReadLe(cursor, code)) {
            OnTransportError();
            return;
        }

        // 🔴 THREE OUTCOMES, NOT TWO. A rejection is the server declining work it cannot queue
        // (§10.8) and it is counted apart from a refusal, which is the game rules saying no. Rolled
        // together they would say "the server broke" about a run in which nothing broke.
        const bool rejected = code == atlas_demo::kEquipResponseUnavailable;
        const bool succeeded = code == static_cast<UInt8>(atlas_demo::EquipResult::Ok);
        if (rejected) {
            ++result_.responses_unavailable;
            NoteRejection();
        } else if (succeeded) {
            ++result_.responses_ok;
        } else {
            ++result_.responses_refused;
            NoteError();
        }

        const UInt32 elapsed_us = static_cast<UInt32>(
            std::chrono::duration_cast<atlas::Micros>(arrived - sent_at_).count());

        // 🔴 Only the steady window is sampled. Connect, the first character load and the driver's
        // first prepare of each statement all land in the warm-up and every one of them is a
        // once-per-run cost that would drag the tail of a short run.
        const std::size_t stage = StageIndexAt(arrived);
        const StageWindow& window = windows_[stage];
        if (arrived >= window.steady_begin && arrived <= window.end) {
            StageSlice& slice = result_.stages[stage];
            slice.latencies_us.push_back(elapsed_us);
            if (rejected) {
                ++slice.responses_rejected;
            } else if (succeeded) {
                ++slice.responses_ok;
                slice.ok_latencies_us.push_back(elapsed_us);
            } else {
                ++slice.responses_refused;
            }
        }
        // 🔴 The live view sees the WHOLE run including the warm-up, because its job is to show the
        // run happening — the transition of §16.1c-① is precisely the thing a discarded prefix
        // would hide. It is a separate histogram and it never feeds the reported percentiles.
        if (metrics_ != nullptr) {
            metrics_->RecordLatency(elapsed_us, succeeded);
        }

        ScheduleNext();
    }

    void ScheduleNext() {
        if (atlas::Clock::now() >= deadline_) {
            Close();
            return;
        }
        if (interval_ == atlas::Duration::zero()) {
            SendEquipRequest();
            return;
        }

        // Paced: the schedule advances by a fixed step rather than from "now", so a slow reply does
        // not silently lower the offered rate for the rest of the run. It is clamped to now so a
        // connection that fell behind does not then fire a burst to catch up.
        const atlas::TimePoint now = atlas::Clock::now();
        next_send_ = next_send_ + interval_;
        if (next_send_ < now) {
            next_send_ = now;
        }

        auto self = shared_from_this();
        timer_.expires_at(next_send_);
        timer_.async_wait(atlas::asio::bind_executor(
            strand_, atlas::Guarded(atlas::Ctx{}, [self](const atlas::ErrorCode& error) {
                if (error || self->closed_) {
                    return;
                }
                if (atlas::Clock::now() >= self->deadline_) {
                    self->Close();
                    return;
                }
                self->SendEquipRequest();
            })));
    }

    void SendEquipRequest() {
        const UInt64 item_uid = item_uids_[next_item_];
        next_item_ = (next_item_ + 1U) % item_uids_.size();

        payload_.clear();
        atlas::generated::WriteLe(payload_, item_uid);
        atlas::generated::WriteLe(payload_, static_cast<UInt8>(kDrivenSlot));
        ++result_.requests_sent;
        SendFrame(atlas_demo::kOpEquipRequest);
    }

    void SendLoadRequest() {
        payload_.clear();
        atlas::generated::WriteLe(payload_, character_id_);
        SendFrame(atlas_demo::kOpCharacterLoadRequest);
    }

    // Encodes `payload_` and puts it on the wire. 🔴 `seq` is owned here, not inside SendFrame's
    // namesake in the core (session_framing.h says why): one send counter per connection, and the
    // layer that decides what to send is the layer that keeps it.
    void SendFrame(UInt16 opcode) {
        ++send_seq_;
        wire_.clear();
        if (!atlas::EncodeFrame(wire_, opcode, send_seq_, payload_)) {
            OnTransportError();
            return;
        }

        // Safe to hold `wire_` across the write: the ping-pong keeps exactly one request in flight.
        sent_at_ = atlas::Clock::now();
        auto self = shared_from_this();
        atlas::asio::async_write(
            socket_, atlas::asio::buffer(wire_),
            atlas::asio::bind_executor(
                strand_,
                atlas::Guarded(atlas::Ctx{}, [self](const atlas::ErrorCode& error, std::size_t) {
                    if (error && !self->closed_) {
                        self->OnTransportError();
                    }
                })));
    }

    void OnTransportError() {
        if (closed_) {
            return;
        }
        if (result_.established) {
            result_.transport_failed = true;
            NoteError();
        }
        Close();
    }

    void Close() {
        if (closed_) {
            return;
        }
        closed_ = true;

        // Discarded explicitly — see atlas/net/acceptor.cpp for why the return value is not enough
        // to leave unnamed.
        atlas::ErrorCode ignored;
        std::ignore = socket_.shutdown(atlas::Socket::shutdown_both, ignored);
        std::ignore = socket_.close(ignored);
        timer_.cancel();

        if (result_.established) {
            live_.fetch_sub(1);
        }
        Finish();
    }

    void Finish() {
        if (finished_) {
            return;
        }
        finished_ = true;
        if (on_finished_) {
            on_finished_();
        }
    }

    // The stage `arrived` belongs to: the last window that had already opened. Linear because a
    // ramp is a handful of stages and a search would be longer than what it replaces.
    [[nodiscard]] std::size_t StageIndexAt(atlas::TimePoint arrived) const noexcept {
        std::size_t index = 0;
        for (std::size_t candidate = 0; candidate < windows_.size(); ++candidate) {
            if (windows_[candidate].begin <= arrived) {
                index = candidate;
            }
        }
        return index;
    }

    void RetryLoadLater() {
        if (atlas::Clock::now() >= deadline_) {
            Close();
            return;
        }
        auto self = shared_from_this();
        timer_.expires_after(kLoadRetryDelay);
        timer_.async_wait(atlas::asio::bind_executor(
            strand_, atlas::Guarded(atlas::Ctx{}, [self](const atlas::ErrorCode& error) {
                if (error || self->closed_) {
                    return;
                }
                if (atlas::Clock::now() >= self->deadline_) {
                    self->Close();
                    return;
                }
                self->SendLoadRequest();
            })));
    }

    // Every failure the run counts is also a tick on the live view's cumulative error line.
    void NoteError() noexcept {
        if (metrics_ != nullptr) {
            metrics_->RecordError();
        }
    }

    // 🔴 Deliberately NOT NoteError. The live view's error line has to stay at zero while the
    // rejection line climbs, because that pair is the whole observation.
    void NoteRejection() noexcept {
        if (metrics_ != nullptr) {
            metrics_->RecordRejection();
        }
    }

    void NoteLoadRejection() noexcept {
        if (metrics_ != nullptr) {
            metrics_->RecordLoadRejection();
        }
    }

    void BumpLive() {
        const std::size_t live = live_.fetch_add(1) + 1;
        std::size_t seen = peak_.load();
        while (live > seen && !peak_.compare_exchange_weak(seen, live)) {
        }
    }

    atlas::Strand strand_;
    atlas::Socket socket_;
    atlas::SteadyTimer timer_;
    atlas::Endpoint endpoint_;

    UInt64 character_id_{0};
    atlas::Duration interval_{};
    // The next paced send. Each connection is given its own starting phase so that N connections at
    // an aggregate rate arrive evenly spread — 🔴 without that they all fire on the same tick and
    // the harness measures its own thundering herd instead of the server (observed 2026-08-10:
    // 64 connections at 40 req/s reported p50 53 ms; evenly phased, the same offered rate is 5 ms).
    atlas::TimePoint next_send_{};
    // When this connection joins the run. `start` for every connection of a fixed-concurrency run,
    // and spread across the joining stage's warm-up prefix in a ramp.
    atlas::TimePoint activate_at_{};
    // Owned by RunLoad, which outlives every connection. Read-only after the run is built.
    const std::vector<StageWindow>& windows_;
    std::optional<bool> no_delay_;
    atlas::TimePoint deadline_{};
    atlas::TimePoint sent_at_{};

    std::atomic<std::size_t>& live_;
    std::atomic<std::size_t>& peak_;
    // 🔴 Null unless --tui or --sample-jsonl asked for a live view, and the null check is the whole
    // cost of the visualisation axis on the default measurement path (§16.1a).
    LiveMetrics* metrics_{nullptr};
    std::function<void()> on_finished_;

    atlas::FrameReader reader_;
    std::vector<atlas::Frame> frames_;
    std::array<Byte, kReadBufferSize> read_buffer_{};
    std::vector<Byte> payload_;
    std::vector<Byte> wire_;

    std::array<UInt64, 2> item_uids_{};
    std::size_t next_item_{0};
    UInt32 send_seq_{0};
    bool closed_{false};
    bool finished_{false};

    ConnectionResult result_;
};

}  // namespace

LoadStats RunLoad(const LoadOptions& options) {
    LoadStats stats;
    stats.connections_attempted = options.connections;
    if (options.connections == 0) {
        return stats;
    }

    atlas::IoContext io_context;
    const atlas::Endpoint endpoint(atlas::asio::ip::make_address(options.host), options.port);

    const atlas::TimePoint start = atlas::Clock::now();
    const atlas::TimePoint deadline = start + atlas::Seconds{options.duration_seconds};

    // 🔴 One window for a fixed-concurrency run, spanning the whole run: that is byte-for-byte the
    // admission rule §16.1's tables were taken with, expressed once instead of twice.
    std::vector<StageWindow> windows;
    if (options.ramp_stages.empty()) {
        windows.push_back(
            StageWindow{.connections = options.connections,
                        .begin = start,
                        .steady_begin = start + atlas::Seconds{options.warmup_seconds},
                        .end = deadline});
    } else {
        for (std::size_t index = 0; index < options.ramp_stages.size(); ++index) {
            const atlas::TimePoint begin =
                start + (atlas::Seconds{options.stage_seconds} * static_cast<Int64>(index));
            windows.push_back(
                StageWindow{.connections = options.ramp_stages[index],
                            .begin = begin,
                            .steady_begin = begin + atlas::Seconds{options.warmup_seconds},
                            .end = begin + atlas::Seconds{options.stage_seconds}});
        }
    }

    UInt64 steady_total_ms = 0;
    for (const StageWindow& window : windows) {
        steady_total_ms += static_cast<UInt64>(
            std::chrono::duration_cast<atlas::Millis>(window.end - window.steady_begin).count());
    }
    stats.steady_window_ms = static_cast<UInt32>(steady_total_ms);

    // A target of R requests/s spread over N connections is one request every N/R seconds each.
    atlas::Duration interval = atlas::Duration::zero();
    if (options.rate_per_second != 0) {
        interval = atlas::Micros{(1000000ULL * static_cast<UInt64>(options.connections)) /
                                 options.rate_per_second};
    }

    std::atomic<std::size_t> live{0};
    std::atomic<std::size_t> peak{0};
    std::atomic<std::size_t> remaining{options.connections};
    std::atomic<bool> watchdog_fired{false};

    // 🔴 Nothing below is allocated, started or checked when neither flag is set — the default run
    // is the one §16.1 measured, unchanged (§16.1a).
    const bool live_view_wanted = options.tui || !options.sample_jsonl_path.empty();
    std::unique_ptr<LiveMetrics> metrics;
    std::unique_ptr<LiveView> live_view;
    if (live_view_wanted) {
        metrics = std::make_unique<LiveMetrics>();
        LiveViewOptions view_options;
        view_options.tui = options.tui;
        view_options.jsonl_path = options.sample_jsonl_path;
        view_options.probe_file_path = options.probe_file_path;
        // The run's own conditions travel with its samples. The ones a run cannot know about
        // itself — host, container, MySQL durability settings — stay in §16.1b and reach the report
        // as notes; a page of curves with no conditions is as unreadable as a table with none.
        std::string ramp;
        for (const std::size_t stage_connections : options.ramp_stages) {
            ramp += ramp.empty() ? "" : ",";
            ramp += std::to_string(stage_connections);
        }
        view_options.meta_json = std::format(
            R"({{"kind":"meta","harness":"atlas_loadgen","connections":{},"rate_per_second":{},)"
            R"("duration_seconds":{},"warmup_seconds":{},"io_threads":{},"host":"{}","port":{},)"
            R"("server_id":{},"first_character_id":{},"ramp":"{}","stage_seconds":{},)"
            R"("sample_interval_ms":1000}})",
            options.connections, options.rate_per_second, options.duration_seconds,
            options.warmup_seconds, options.io_threads, options.host, options.port,
            options.server_id, options.first_character_id, ramp, options.stage_seconds);
        for (const StageWindow& window : windows) {
            view_options.stages.push_back(StageMark{
                .begins_at_seconds =
                    static_cast<atlas::Float64>(
                        std::chrono::duration_cast<atlas::Millis>(window.begin - start).count()) /
                    1000.0,
                .connections = window.connections});
        }
        live_view = std::make_unique<LiveView>(std::move(view_options), *metrics, live);
    }

    atlas::SteadyTimer watchdog(io_context);
    watchdog.expires_at(deadline + kWatchdogGrace);

    // When each connection joins. 🔴 In a ramp the connections that a stage adds are spread evenly
    // across that stage's warm-up prefix rather than connecting together: a simultaneous connect +
    // character load from N clients is a thundering herd, and §16.1a already records one
    // measurement this harness lost to exactly that shape. Fixed-concurrency runs are untouched —
    // every connection still starts at `start`.
    std::vector<atlas::TimePoint> activations(options.connections, start);
    if (!options.ramp_stages.empty()) {
        const auto spread =
            std::chrono::duration_cast<atlas::Duration>(atlas::Seconds{options.warmup_seconds});
        std::size_t already_joined = 0;
        for (const StageWindow& window : windows) {
            const std::size_t joining = window.connections - already_joined;
            for (std::size_t offset = 0; offset < joining; ++offset) {
                activations[already_joined + offset] =
                    window.begin + ((spread * static_cast<atlas::Duration::rep>(offset)) /
                                    static_cast<atlas::Duration::rep>(joining));
            }
            already_joined = window.connections;
        }
    }

    std::vector<std::shared_ptr<LoadConnection>> connections;
    connections.reserve(options.connections);
    for (std::size_t index = 0; index < options.connections; ++index) {
        // Even phase spread across the interval: connection i starts one Nth of a step after its
        // predecessor, so an aggregate rate arrives as a steady stream rather than as one burst per
        // interval.
        const atlas::TimePoint first_send =
            start + ((interval * static_cast<atlas::Duration::rep>(index)) /
                     static_cast<atlas::Duration::rep>(options.connections));
        connections.push_back(std::make_shared<LoadConnection>(
            io_context, endpoint, options.first_character_id + static_cast<UInt64>(index), interval,
            first_send, activations[index], windows, options.no_delay, live, peak, metrics.get(),
            [&io_context, &remaining, &watchdog] {
                if (remaining.fetch_sub(1) != 1) {
                    return;
                }
                // The last connection is done, so the watchdog is the only thing left holding
                // io_context::run(). Cancelling it from its own context is what lets run() return
                // instead of always paying the grace period.
                atlas::asio::post(io_context,
                                  atlas::Guarded(atlas::Ctx{}, [&watchdog] { watchdog.cancel(); }));
            }));
    }

    // 🔴 The run must end even if a connection never answers: without this the harness would hang
    // exactly when the server is at its worst, which is the one case it exists to report on.
    watchdog.async_wait(atlas::Guarded(
        atlas::Ctx{}, [&watchdog_fired, &connections](const atlas::ErrorCode& error) {
            if (error) {
                return;
            }
            watchdog_fired.store(true);
            for (const std::shared_ptr<LoadConnection>& connection : connections) {
                connection->Stop();
            }
        }));

    if (live_view) {
        live_view->Start();
    }

    for (const std::shared_ptr<LoadConnection>& connection : connections) {
        connection->Start();
    }

    const std::size_t thread_count = options.io_threads == 0 ? std::size_t{1} : options.io_threads;
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (std::size_t index = 0; index < thread_count; ++index) {
        workers.emplace_back([&io_context] { io_context.run(); });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    if (live_view) {
        live_view->Stop();
    }

    stats.stages.resize(windows.size());
    for (std::size_t index = 0; index < windows.size(); ++index) {
        stats.stages[index].connections = windows[index].connections;
        stats.stages[index].window_ms =
            static_cast<UInt32>(std::chrono::duration_cast<atlas::Millis>(
                                    windows[index].end - windows[index].steady_begin)
                                    .count());
    }

    for (const std::shared_ptr<LoadConnection>& connection : connections) {
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
        for (std::size_t index = 0; index < windows.size(); ++index) {
            const StageSlice& slice = result.stages[index];
            StageStats& stage = stats.stages[index];
            stage.responses_ok += slice.responses_ok;
            stage.responses_rejected += slice.responses_rejected;
            stage.responses_refused += slice.responses_refused;
            stage.latencies_us.insert(stage.latencies_us.end(), slice.latencies_us.begin(),
                                      slice.latencies_us.end());
            stage.ok_latencies_us.insert(stage.ok_latencies_us.end(), slice.ok_latencies_us.begin(),
                                         slice.ok_latencies_us.end());
        }
    }
    for (const StageStats& stage : stats.stages) {
        stats.latencies_us.insert(stats.latencies_us.end(), stage.latencies_us.begin(),
                                  stage.latencies_us.end());
    }
    stats.peak_live_connections = peak.load();
    stats.watchdog_fired = watchdog_fired.load();
    return stats;
}

UInt32 Percentile(const std::vector<UInt32>& sorted_samples, atlas::Float64 fraction) {
    if (sorted_samples.empty()) {
        return 0;
    }
    // Nearest rank: the smallest sample at or above the requested fraction of the population. No
    // interpolation — an interpolated p99 invents a value nothing measured.
    const auto count = static_cast<atlas::Float64>(sorted_samples.size());
    const atlas::Float64 exact_rank = std::ceil(fraction * count);
    std::size_t rank = 1;
    if (exact_rank > 1.0) {
        rank = static_cast<std::size_t>(exact_rank);
    }
    if (rank > sorted_samples.size()) {
        rank = sorted_samples.size();
    }
    return sorted_samples[rank - 1];
}

}  // namespace atlas_loadgen
