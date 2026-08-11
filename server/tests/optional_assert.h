#pragma once
#include <gtest/gtest.h>

// Test-only assertion that an optional holds a value, written so that clang-tidy's dataflow can
// see it.
//
// 🔴 Why this exists rather than a plain ASSERT_TRUE(x.has_value()): the two are equivalent at run
// time, but `bugprone-unchecked-optional-access` cannot follow gtest's macro. ASSERT_TRUE expands
// to `if (const AssertionResult gtest_ar_ = AssertionResult(cond)) ; else return ...`, and the
// analysis has no way to connect that opaque AssertionResult back to the `has_value()` it was
// built from — so every deref after it was reported as unchecked. Suppressing the check in tests
// was the alternative, and it would have blinded the suites to the real thing the check is for.
//
// This form keeps the runtime behaviour identical (FAIL() is a fatal gtest failure that returns
// from the test body) while the `if (!cond) { return; }` shape is exactly what the model reads.
// cpp-style.md §5 — the ATLAS_ prefix is what makes this a sanctioned macro rather than a stray
// one, and `cppcoreguidelines-macro-usage.AllowedRegexp` in .clang-tidy enforces that.
//
// 🔴 Only usable inside a void-returning test body, because FAIL() is.
#define ATLAS_ASSERT_HAS_VALUE(opt)         \
    do {                                    \
        if (!(opt).has_value()) {           \
            FAIL() << #opt " has no value"; \
        }                                   \
    } while (false)
