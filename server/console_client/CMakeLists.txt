# The interactive console client (architecture-design.md §15.3 — the "compose up, then drive it by
# hand" path).
#
# 🔴 It links atlas_demo for the same reason the load harness does: the wire it speaks is THIS
# game's opcode table and payload order (game/handlers.h), and a client with its own copy would
# drift from the server it is talking to. The edge points console_client -> game -> core and never
# returns (§14).
#
# 🔴 It is a client, so it is deliberately NOT part of the deployed runtime stage of
# server/Dockerfile — the same rule the load harness follows. It connects from outside the stack to
# the port compose publishes.
#
# 🔴 No libmariadb plugin-directory workaround here, unlike loadgen and the test targets: this
# binary opens no database connection. It only ever speaks §8.1 frames to the GAME server, and the
# server owns the database (§5.1).
add_executable(atlas_console console_client.cpp main.cpp)
target_link_libraries(atlas_console PRIVATE atlas_demo)
