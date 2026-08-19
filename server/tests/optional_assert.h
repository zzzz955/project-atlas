#pragma once

// =============================================================================
// optional 이 값을 가짐을 단언하는 테스트 전용 매크로
// clang-tidy 의 dataflow 가 읽을 수 있는 if (!cond) return 형태로 씀
// =============================================================================

#include <gtest/gtest.h>

// [CS 5] ASSERT_TRUE( x.has_value() ) 를 대신한다. void 반환 본문 전용
// 그쪽은 bugprone-unchecked-optional-access 가 이후 deref 를 전부 미검사로 신고한다
// 호출식이 아니라 이름 붙인 변수를 넘길 것. 검사는 저장 위치에 표시를 단다
// EXPECT_/ASSERT_ 본문 안에서 deref 하지 말 것. 매크로 전개가 그 위치를 잃는다
// [AD 15.5i] MSVC STL 은 operator* 를 신고하지 않아 CI 왕복으로만 드러난다
#define ATLAS_ASSERT_HAS_VALUE( opt )       \
    do                                      \
    {                                       \
        if ( !( opt ).has_value() )         \
        {                                   \
            FAIL() << #opt " has no value"; \
        }                                   \
    } while ( false )
