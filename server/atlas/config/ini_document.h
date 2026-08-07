#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// architecture-design.md §5.4 — the config sources are two and they do not mix. This file owns
// the committed half: `server/server.ini` (role / ids / ports / workers / log). The secret half
// is `.env`, loaded by config/secret_config.h.
//
// 🔴 The parser is hand-written on purpose. The grammar this project needs is `[section]`,
// `key = value`, `;`/`#` comments and blank lines — under a hundred lines. Reaching for
// Boost.PropertyTree would make a seventh entry in server/vcpkg.json (§15.2) and buy nothing.

namespace atlas {

class IniDocument {
public:
    // 🔴 Malformed input is a failure, never a silent skip: a typo in a role or a port is exactly
    // the kind of mistake that otherwise boots a server into the wrong identity (§5.2). Throws
    // atlas::Exception — a start-up config that cannot be read is unrecoverable state, which is the
    // narrow case §11.2(a) reserves exceptions for.
    [[nodiscard]] static IniDocument Parse(std::string_view text);
    [[nodiscard]] static IniDocument LoadFile(std::string_view path);

    // The returned view is owned by this document.
    [[nodiscard]] std::optional<std::string_view> Get(std::string_view section,
                                                      std::string_view key) const;

private:
    // std::less<> makes the maps transparent, so a std::string_view looks a key up without
    // materialising a std::string first.
    using Section = std::map<std::string, std::string, std::less<>>;

    std::map<std::string, Section, std::less<>> sections_;
};

}  // namespace atlas
