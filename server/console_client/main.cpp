#include <charconv>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atlas/core/error.h"
#include "atlas/core/log.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"
#include "atlas/net/io_runner.h"
#include "atlas/net/net_types.h"
#include "console_client/console_client.h"

// The interactive console client binary (architecture-design.md §15.3).
//
//     atlas_console --host 127.0.0.1 --port 7777
//
// 🔴 THE TARGET IS AN ARGUMENT, NEVER BAKED IN. A host and a port are not secrets (§5.4 draws that
// line at credentials), so they are plain arguments rather than .env keys — but a client that could
// only ever reach one address would be useless the first time the stack moved.
//
// 🔴 THIS THREAD OWNS THE TERMINAL AND BLOCKS ON IT. std::getline blocks, and a blocking read on an
// I/O thread stops every session that thread serves (§9, and §10.3 isolates the blocking database
// calls for the same reason). So the process is inverted from the server's shape: asio runs on a
// worker pool of its own and main() is the one thread allowed to wait on a human.
//
// 🔴 EXACTLY FIVE COMMANDS, AND NOTHING THAT DRAWS. Each one is a real §8.1 frame over a real
// socket; there is no command here that the GAME server's opcode table (game/handlers.h) does not
// answer, because a command that printed a locally invented reply would demo the client rather than
// the server.

namespace {

using atlas::UInt16;
using atlas::UInt64;
using atlas::UInt8;

// How long the REPL waits for one round trip before it gives up. On a local stack every one of
// these is a few milliseconds; ten seconds means something is genuinely wrong, and hanging the
// terminal would hide that rather than report it.
constexpr atlas::Seconds kRequestTimeout{10};

constexpr UInt16 kDefaultRankCount = 10;

[[nodiscard]] std::optional<UInt64> ParseUnsigned(std::string_view text) {
    UInt64 value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const std::from_chars_result parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::vector<std::string_view> Split(const std::string& line) {
    std::vector<std::string_view> tokens;
    const std::string_view view(line);
    std::size_t cursor = 0;
    while (cursor < view.size()) {
        const std::size_t begin = view.find_first_not_of(" \t\r", cursor);
        if (begin == std::string_view::npos) {
            break;
        }
        std::size_t end = view.find_first_of(" \t\r", begin);
        if (end == std::string_view::npos) {
            end = view.size();
        }
        tokens.push_back(view.substr(begin, end - begin));
        cursor = end;
    }
    return tokens;
}

void PrintUsage() {
    std::printf(
        "commands:\n"
        "  load <character_id>        load the character and bind this connection to it\n"
        "  inv                        re-read the bound character and print its items\n"
        "  equip <item_uid> <slot>    equip an item into slot 1 (weapon) / 2 (armor) / 3\n"
        "  rank [count]               top-N by exp, straight out of the cache\n"
        "  quit                       close the connection and exit\n");
}

// Issues one request and blocks THIS thread until the client reports the round trip finished.
// 🔴 The wait is on the REPL thread and never on an I/O thread; the completion that releases it
// runs on the connection strand, and ConsoleClient::Close fires it too, so a dropped connection
// ends the wait instead of extending it forever.
[[nodiscard]] bool Await(const std::function<void(atlas_console::Completion)>& issue) {
    auto answered = std::make_shared<std::promise<void>>();
    std::future<void> waiter = answered->get_future();
    issue([answered] { answered->set_value(); });
    if (waiter.wait_for(kRequestTimeout) == std::future_status::timeout) {
        std::printf("no answer within %us — giving up on this connection\n",
                    static_cast<atlas::UInt32>(kRequestTimeout.count()));
        return false;
    }
    return true;
}

// Returns false when the session can no longer continue (the caller then leaves the loop).
[[nodiscard]] bool RunCommand(const std::shared_ptr<atlas_console::ConsoleClient>& client,
                              const std::vector<std::string_view>& tokens) {
    const std::string_view command = tokens.front();

    if (command == "load") {
        if (tokens.size() != 2) {
            std::printf("usage: load <character_id>\n");
            return true;
        }
        const std::optional<UInt64> character_id = ParseUnsigned(tokens[1]);
        if (!character_id.has_value()) {
            std::printf("'%s' is not a whole number\n", std::string(tokens[1]).c_str());
            return true;
        }
        return Await([&client, character_id](atlas_console::Completion done) {
            client->RequestLoad(*character_id, std::move(done));
        });
    }

    if (command == "inv") {
        return Await([&client](atlas_console::Completion done) {
            client->RequestInventory(std::move(done));
        });
    }

    if (command == "equip") {
        if (tokens.size() != 3) {
            std::printf("usage: equip <item_uid> <slot>\n");
            return true;
        }
        const std::optional<UInt64> item_uid = ParseUnsigned(tokens[1]);
        const std::optional<UInt64> slot = ParseUnsigned(tokens[2]);
        if (!item_uid.has_value() || !slot.has_value() || *slot > 0xFFU) {
            std::printf("usage: equip <item_uid> <slot>, both whole numbers\n");
            return true;
        }
        // 🔴 An out-of-range or mismatched slot is NOT filtered here. Server authority is the whole
        // point of §8.2 layer 3, and a client that pre-validated would hide the check under test.
        return Await([&client, item_uid, slot](atlas_console::Completion done) {
            client->RequestEquip(*item_uid, static_cast<UInt8>(*slot), std::move(done));
        });
    }

    if (command == "rank") {
        UInt16 count = kDefaultRankCount;
        if (tokens.size() == 2) {
            const std::optional<UInt64> requested = ParseUnsigned(tokens[1]);
            if (!requested.has_value() || *requested > 0xFFFFU) {
                std::printf("usage: rank [count]\n");
                return true;
            }
            count = static_cast<UInt16>(*requested);
        }
        return Await([&client, count](atlas_console::Completion done) {
            client->RequestRanking(count, std::move(done));
        });
    }

    std::printf("unknown command '%s'\n", std::string(command).c_str());
    PrintUsage();
    return true;
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT — the standard fixes main's signature.
    try {
        // Console only, warnings and worse: everything this tool has to say it says on stdout, and
        // a client with its own log file would just be another writer on the demo machine.
        atlas::LogConfig log_config;
        log_config.level = atlas::LogLevel::Warn;
        atlas::LogInit(log_config);

        std::string host = "127.0.0.1";
        UInt16 port = 7777;
        for (std::size_t index = 1; index + 1 < static_cast<std::size_t>(argc); index += 2) {
            const std::string_view key(argv[index]);
            const std::string_view value(argv[index + 1]);
            if (key == "--host") {
                host = std::string(value);
            } else if (key == "--port") {
                const std::optional<UInt64> parsed = ParseUnsigned(value);
                ATLAS_CHECK(parsed.has_value() && *parsed <= 0xFFFFU, "'{}' is not a port", value);
                port = static_cast<UInt16>(*parsed);
            } else {
                ATLAS_THROW(atlas::Exception, "unknown option '{}'", key);
            }
        }

        // One worker: this client holds one connection, and a pool would be a claim about
        // concurrency that nothing here exercises.
        atlas::IoRunner runner(1);
        const atlas::Endpoint endpoint(atlas::asio::ip::make_address(host), port);
        auto client = std::make_shared<atlas_console::ConsoleClient>(runner.Context(), endpoint);
        runner.Start();

        if (!Await(
                [&client](atlas_console::Completion done) { client->Connect(std::move(done)); }) ||
            !client->Alive()) {
            client->Shutdown();
            runner.Stop();
            atlas::LogShutdown();
            return 69;
        }

        PrintUsage();
        std::string line;
        while (client->Alive()) {
            std::printf("atlas> ");
            std::fflush(stdout);
            // 🔴 EOF (a piped script running out, or Ctrl+Z / Ctrl+D) is a quit, not an error: the
            // alternative is a REPL that spins on a closed stdin.
            if (!std::getline(std::cin, line)) {
                break;
            }
            const std::vector<std::string_view> tokens = Split(line);
            if (tokens.empty()) {
                continue;
            }
            if (tokens.front() == "quit") {
                break;
            }
            if (!RunCommand(client, tokens)) {
                break;
            }
        }

        // §9 shutdown order, minus the acceptor this process does not have: close the connection
        // first so the outstanding read completes, then let the queue drain.
        client->Shutdown();
        runner.Stop();
        std::printf("bye\n");
        atlas::LogShutdown();
        return 0;
    } catch (const std::exception& failure) {
        std::printf("console client failed: %s\n", failure.what());
        atlas::LogShutdown();
        return 70;
    }
}
