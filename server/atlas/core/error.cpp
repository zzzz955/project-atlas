#include "atlas/core/error.h"

#include "atlas/core/stack_trace.h"

namespace atlas {
namespace {

// The decoration lives in what(), not only in the log line, because an exception that crosses a
// layer boundary and is reported by somebody else must still say where it came from.
std::string DecorateMessage(const std::string& message, const std::source_location& where,
                            UInt64 trace_id) {
    return std::format("{} [trace={}] at {}:{} in {}", message, trace_id, where.file_name(),
                       where.line(), where.function_name());
}

}  // namespace

Exception::Exception(const std::string& message, std::source_location where)
    : std::runtime_error(DecorateMessage(message, where, CurrentCtx().trace_id)),
      where_(where),
      trace_id_(CurrentCtx().trace_id),
      stack_trace_(CaptureStackTrace(1)) {}

namespace detail {

void ReportGuardedException(const std::exception& failure) noexcept {
    try {
        std::string trace;
        if (const auto* atlas_failure = dynamic_cast<const Exception*>(&failure);
            atlas_failure != nullptr) {
            trace = std::string(atlas_failure->StackTrace());
        } else {
            trace = CaptureCurrentExceptionStackTrace();
        }

        if (trace.empty()) {
            ATLAS_LOG_ERROR("guarded handler threw: {} [stack unavailable]", failure.what());
        } else {
            ATLAS_LOG_ERROR("guarded handler threw: {}\nthrow stack:\n{}", failure.what(), trace);
        }
    } catch (...) {  // NOLINT — the reporting path must be total.
    }
}

void ReportGuardedUnknownException() noexcept {
    try {
        const std::string trace = CaptureCurrentExceptionStackTrace();
        if (trace.empty()) {
            ATLAS_LOG_ERROR("guarded handler threw: non-std exception [stack unavailable]");
        } else {
            ATLAS_LOG_ERROR("guarded handler threw: non-std exception\nthrow stack:\n{}", trace);
        }
    } catch (...) {  // NOLINT — see ReportGuardedException.
    }
}

}  // namespace detail

}  // namespace atlas
