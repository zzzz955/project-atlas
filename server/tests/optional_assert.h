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
//
// 🔴 PASS IT A NAMED VARIABLE, NEVER A CALL EXPRESSION. The analysis attaches "this one holds a
// value" to a storage location, and `r.values.front()` written twice is two locations to it — so
// guarding the call and then dereferencing the call again leaves the deref unchecked. Bind first:
//
//     const std::optional<T>& value = r.values.front();
//     ATLAS_ASSERT_HAS_VALUE(value);
//     EXPECT_EQ(*value, expected);
//
// This cost a CI round trip (architecture-design.md §15.5i): the local sweep could not catch it,
// because MSVC's STL never reports `operator*` for this check at all.
#define ATLAS_ASSERT_HAS_VALUE(opt)         \
    do {                                    \
        if (!(opt).has_value()) {           \
            FAIL() << #opt " has no value"; \
        }                                   \
    } while (false)
