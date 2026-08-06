#include "atlas/core/error.h"

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
      trace_id_(CurrentCtx().trace_id) {}

}  // namespace atlas
