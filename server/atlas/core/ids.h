#pragma once
#include <concepts>

#include "atlas/core/types.h"

namespace atlas {

// cpp-style.md §4.3 — strong-typed IDs. The cheapest rule with the biggest payoff.
//
// If every identifier is just a `UInt64` alias, `Foo(session_id, actor_id)` still compiles when the
// two arguments are swapped and then goes silently wrong at runtime. `enum class` has no implicit
// conversion, so the same mistake becomes a compile error — and it costs nothing at runtime.
//
// architecture-design.md §6 — the identity ladder is account -> server -> character. `server_id` is
// deliberately NOT a strong type yet: it is not carried around as an argument anywhere, and a type
// nobody passes by mistake buys nothing. Promote it when a call site actually needs the guard.
enum class AccountId : UInt64 {};
enum class CharacterId : UInt64 {};
enum class SessionId : UInt64 {};
enum class ActorId : UInt64 {};

// The single explicit way back to the raw value. Keeping extraction to one named function means
// `static_cast` never has to appear at call sites, so a stray cast in review is a real smell.
// The concept restricts it to the four framework IDs (cpp-style.md §6 — constraints are concepts,
// never SFINAE), so it cannot be used to launder an arbitrary enumeration.
template <class T>
concept StrongId = std::same_as<T, AccountId> || std::same_as<T, CharacterId> ||
                   std::same_as<T, SessionId> || std::same_as<T, ActorId>;

template <StrongId Id>
constexpr UInt64 IdValue(Id id) noexcept {
    return static_cast<UInt64>(id);
}

}  // namespace atlas
