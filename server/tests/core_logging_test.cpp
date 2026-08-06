#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>
#include <type_traits>

#include "atlas/core/ctx.h"
#include "atlas/core/error.h"
#include "atlas/core/ids.h"
#include "atlas/core/log.h"
#include "atlas/core/types.h"

namespace {

// Stands in for the `ExpensiveDump(a)` of cpp-style.md §5: the whole argument for the log macros is
// that this counter must stay at zero when the level is off.
atlas::UInt64 g_log_probe_calls = 0;

atlas::UInt64 LogProbe() {
    ++g_log_probe_calls;
    return g_log_probe_calls;
}

// 🔴 This is THE test that justifies the macros existing at all (cpp-style.md §5). If a refactor
// ever turns the log entry points into functions, this is what fails.
TEST(CoreLogging, DisabledLevelDoesNotEvaluateItsArguments) {
    atlas::SetLogLevel(atlas::LogLevel::Info);
    g_log_probe_calls = 0;
    const atlas::UInt64 debug_records_before = atlas::LogCount(atlas::LogLevel::Debug);

    ATLAS_LOG_DEBUG("probe={}", LogProbe());

    EXPECT_EQ(g_log_probe_calls, atlas::UInt64{0});
    EXPECT_EQ(atlas::LogCount(atlas::LogLevel::Debug), debug_records_before);
}

TEST(CoreLogging, EnabledLevelEvaluatesItsArgumentsExactlyOnce) {
    // The mirror of the test above: without it, "the counter stayed at zero" could just mean the
    // probe never counts anything.
    atlas::SetLogLevel(atlas::LogLevel::Info);
    g_log_probe_calls = 0;
    const atlas::UInt64 info_records_before = atlas::LogCount(atlas::LogLevel::Info);

    ATLAS_LOG_INFO("probe={}", LogProbe());

    EXPECT_EQ(g_log_probe_calls, atlas::UInt64{1});
    EXPECT_EQ(atlas::LogCount(atlas::LogLevel::Info), info_records_before + 1);
}

TEST(CoreLogging, LevelOffSilencesEveryChannel) {
    atlas::SetLogLevel(atlas::LogLevel::Off);
    EXPECT_FALSE(atlas::LogEnabled(atlas::LogLevel::Fatal));
    EXPECT_FALSE(atlas::LogEnabled(atlas::LogLevel::Error));
    EXPECT_EQ(atlas::CurrentLogLevel(), atlas::LogLevel::Off);
    atlas::SetLogLevel(atlas::LogLevel::Info);
    EXPECT_TRUE(atlas::LogEnabled(atlas::LogLevel::Error));
    EXPECT_FALSE(atlas::LogEnabled(atlas::LogLevel::Debug));
}

TEST(CoreLogging, InitWritesThroughTheAsyncFileSink) {
    // Exercises the real spdlog chain: async logger -> daily rolling sink -> file. Anything less
    // and LogInit would be untested code that only breaks in production.
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "atlas_core_logging_test";
    std::filesystem::remove_all(dir);

    atlas::LogConfig config;
    config.directory = dir.string();
    config.basename = "atlas_test";
    config.console = false;
    config.level = atlas::LogLevel::Info;
    atlas::LogInit(config);

    ATLAS_LOG_INFO("file sink smoke test {}", 1);
    atlas::LogFlush();
    atlas::LogShutdown();

    ASSERT_TRUE(std::filesystem::exists(dir));
    EXPECT_FALSE(std::filesystem::is_empty(dir));

    std::filesystem::remove_all(dir);
    atlas::SetLogLevel(atlas::LogLevel::Info);
}

// 🔴 architecture-design.md §11.2(b) — the guard is what keeps one bad handler from killing an I/O
// thread and silently freezing every session it served.
TEST(CoreGuarded, ContainsExceptionsLogsOneErrorAndRunsTheFailureCallback) {
    atlas::SetLogLevel(atlas::LogLevel::Info);
    const atlas::UInt64 errors_before = atlas::LogCount(atlas::LogLevel::Error);

    atlas::UInt64 closed = 0;
    atlas::Ctx ctx;
    ctx.trace_id = 42;

    auto guarded = atlas::Guarded(
        ctx, [] { ATLAS_THROW(atlas::Exception, "boom {}", 7); },
        atlas::FailureHandler{[&closed](std::string_view) { ++closed; }});

    static_assert(noexcept(guarded()), "the guard must be noexcept - it is the last line");
    EXPECT_NO_THROW(guarded());

    EXPECT_EQ(closed, atlas::UInt64{1});
    EXPECT_EQ(atlas::LogCount(atlas::LogLevel::Error), errors_before + 1);
}

TEST(CoreGuarded, ContainsThrowsThatAreNotStdExceptions) {
    atlas::SetLogLevel(atlas::LogLevel::Info);
    atlas::UInt64 closed = 0;
    atlas::Ctx ctx;

    auto guarded = atlas::Guarded(
        ctx, [] { throw 7; }, atlas::FailureHandler{[&closed](std::string_view) { ++closed; }});
    EXPECT_NO_THROW(guarded());
    EXPECT_EQ(closed, atlas::UInt64{1});
}

TEST(CoreGuarded, ForwardsArgumentsAndWorksWithoutAFailureCallback) {
    atlas::UInt64 seen = 0;
    atlas::Ctx ctx;
    atlas::Guarded(ctx, [&seen](atlas::UInt64 value) { seen = value; })(atlas::UInt64{5});
    EXPECT_EQ(seen, atlas::UInt64{5});

    // No callback injected: the guard must still swallow the throw rather than terminate.
    EXPECT_NO_THROW(atlas::Guarded(ctx, [] { ATLAS_THROW(atlas::Exception, "no callback"); })());
}

// 🔴 architecture-design.md §9.2 — a plain thread_local would pass a single-handler test and fail
// the moment handlers nest. This is the test that pins install-on-entry / restore-on-exit.
TEST(CoreCtx, GuardedInstallsTheLedgerAndRestoresTheOuterOneOnExit) {
    atlas::Ctx outer;
    outer.trace_id = 1;
    outer.session_id = atlas::SessionId{11};
    outer.character_id = atlas::CharacterId{22};

    const atlas::CtxScope scope(outer);
    ASSERT_EQ(atlas::CurrentCtx().trace_id, atlas::UInt64{1});

    atlas::Ctx inner;
    inner.trace_id = 2;
    inner.session_id = atlas::SessionId{99};

    atlas::UInt64 seen_trace = 0;
    atlas::UInt64 seen_session = 0;
    atlas::Guarded(inner, [&seen_trace, &seen_session] {
        seen_trace = atlas::CurrentCtx().trace_id;
        seen_session = atlas::IdValue(atlas::CurrentCtx().session_id);
    })();

    EXPECT_EQ(seen_trace, atlas::UInt64{2});
    EXPECT_EQ(seen_session, atlas::UInt64{99});
    EXPECT_EQ(atlas::CurrentCtx().trace_id, atlas::UInt64{1});
    EXPECT_EQ(atlas::IdValue(atlas::CurrentCtx().session_id), atlas::UInt64{11});
    EXPECT_EQ(atlas::IdValue(atlas::CurrentCtx().character_id), atlas::UInt64{22});

    // Restoration must survive the unwinding path too, or one thrown handler poisons the ledger of
    // every handler that runs after it on that thread.
    atlas::Guarded(inner, [] { ATLAS_THROW(atlas::Exception, "boom"); })();
    EXPECT_EQ(atlas::CurrentCtx().trace_id, atlas::UInt64{1});
    EXPECT_EQ(atlas::IdValue(atlas::CurrentCtx().session_id), atlas::UInt64{11});
}

TEST(CoreCtx, DefaultLedgerIsEmpty) {
    EXPECT_EQ(atlas::CurrentCtx().trace_id, atlas::UInt64{0});
    EXPECT_EQ(atlas::CurrentCtx().tx_state, atlas::TxState::None);
}

// cpp-style.md §4.3 — the guard exists so that swapping two ID arguments is a compile error.
TEST(CoreIds, StrongIdsHaveNoImplicitConversions) {
    static_assert(!std::is_convertible_v<atlas::ActorId, atlas::UInt64>);
    static_assert(!std::is_convertible_v<atlas::UInt64, atlas::ActorId>);
    static_assert(!std::is_convertible_v<atlas::AccountId, atlas::CharacterId>);
    static_assert(!std::is_convertible_v<atlas::SessionId, atlas::ActorId>);
    static_assert(sizeof(atlas::ActorId) == sizeof(atlas::UInt64));

    EXPECT_EQ(atlas::IdValue(atlas::ActorId{7}), atlas::UInt64{7});
    EXPECT_EQ(atlas::IdValue(atlas::AccountId{8}), atlas::UInt64{8});
}

TEST(CoreError, ThrowAndCheckCarrySourceLocationAndTrace) {
    atlas::Ctx ctx;
    ctx.trace_id = 1234;
    const atlas::CtxScope scope(ctx);

    // Deliberately non-const: a `const` integer initialised from a literal is still a constant
    // expression, and /W4 then reports C4127 on the condition inside ATLAS_CHECK.
    atlas::Int32 expected = 1;
    atlas::Int32 actual = 2;

    try {
        ATLAS_CHECK(expected == actual, "mismatch {} vs {}", expected, actual);
        FAIL() << "ATLAS_CHECK did not throw";
    } catch (const atlas::Exception& ex) {
        EXPECT_EQ(ex.TraceId(), atlas::UInt64{1234});
        EXPECT_NE(std::string_view{ex.what()}.find("mismatch 1 vs 2"), std::string_view::npos);
        EXPECT_NE(std::string_view{ex.Where().file_name()}, std::string_view{});
    }

    // A passing condition must not throw, and ATLAS_ASSERT must compile in both configurations.
    EXPECT_NO_THROW(ATLAS_CHECK(expected < actual, "never"));
    EXPECT_NO_THROW(ATLAS_ASSERT(expected < actual));
}

}  // namespace
