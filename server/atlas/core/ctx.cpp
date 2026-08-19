// =============================================================================
// ctx 원장의 thread_local 저장소와 RAII 설치/복원
// =============================================================================

#include "atlas/core/ctx.h"

namespace atlas
{
namespace
{

// CtxScope 외에는 쓰지 않으므로 진입과 이탈이 항상 짝을 이룸
thread_local Ctx t_current_ctx{};

}  // namespace

const Ctx& CurrentCtx() noexcept { return t_current_ctx; }

CtxScope::CtxScope( const Ctx& ctx ) noexcept : previous_( t_current_ctx ) { t_current_ctx = ctx; }

CtxScope::~CtxScope() { t_current_ctx = previous_; }

}  // namespace atlas
