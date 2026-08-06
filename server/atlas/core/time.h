#pragma once
#include <chrono>

namespace atlas {

using Clock = std::chrono::steady_clock;  // 단조 — 틱 · 타임아웃 · 지연 측정
using TimePoint = Clock::time_point;
using Duration = Clock::duration;

using SysClock = std::chrono::system_clock;  // 벽시계 — DB 저장 · 로그 타임스탬프
using SysTime = SysClock::time_point;

using Nanos = std::chrono::nanoseconds;
using Micros = std::chrono::microseconds;
using Millis = std::chrono::milliseconds;
using Seconds = std::chrono::seconds;

}  // namespace atlas
