#pragma once
#include <cstddef>
#include <string>

// architecture-design.md §11.1 — symbolized stack traces.
//
// Boost.Stacktrace stays behind this out-of-line API for the same reason spdlog stays behind
// LogWrite: this header is part of the core surface and must not make every consumer parse a heavy
// third-party implementation header.

namespace atlas {

// Captures the current call path. `skip` counts callers above this API; a failure to capture or
// symbolize is represented by an empty string because diagnostics must never replace the original
// failure with another exception.
[[nodiscard]] std::string CaptureStackTrace(std::size_t skip = 0) noexcept;

// Called while a catch block is active. Boost's from-exception backend preserves the throw-site
// path even though normal C++ unwinding has already removed those frames.
[[nodiscard]] std::string CaptureCurrentExceptionStackTrace() noexcept;

}  // namespace atlas
