#include "atlas/core/ctx.h"

namespace atlas {
namespace {

// The slot is thread_local because a handler only ever reads the ledger of the thread it is running
// on. It is never written except through CtxScope, so entry/exit stays balanced.
thread_local Ctx t_current_ctx{};

}  // namespace

const Ctx& CurrentCtx() noexcept { return t_current_ctx; }

CtxScope::CtxScope(const Ctx& ctx) noexcept : previous_(t_current_ctx) { t_current_ctx = ctx; }

CtxScope::~CtxScope() { t_current_ctx = previous_; }

}  // namespace atlas
