# project-atlas — CMake root (architecture-design.md §15.1). CMake is the build SoT.
cmake_minimum_required(VERSION 3.25)

# CMAKE_TOOLCHAIN_FILE (vcpkg) must be in the cache before project() runs — CMakePresets.json
# sets it. Do not set it here and do not hardcode a vcpkg path.
project(project-atlas CXX)

# Included in every crash log and Windows dump filename so an incident cannot be symbolized with a
# merely similar binary. Docker passes the image revision; local builds default to an explicit dev
# marker rather than inventing a commit identity.
set(ATLAS_BUILD_ID "dev" CACHE STRING "Build identifier recorded in crash diagnostics")

# clang-tidy consumes compile_commands.json, and only the Ninja generator emits it — this is why
# every preset uses Ninja rather than the Visual Studio generator (cpp-style.md §7.3).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# §15.1 — CMAKE_UNITY_BUILD itself is flipped per preset (ON locally/release, OFF in the CI gate).
# Only the batch size is fixed here.
set(CMAKE_UNITY_BUILD_BATCH_SIZE 8)

enable_testing()

add_subdirectory(atlas/core)
add_subdirectory(atlas/config)
add_subdirectory(atlas/net)
# architecture-design.md §8.1 — the frame layer sits above atlas/net and links it, never the reverse
# (see atlas/proto/CMakeLists.txt), so it is added after it.
add_subdirectory(atlas/proto)
# architecture-design.md §10 — the ORM runtime. Independent of atlas/net: the DB thread pool is
# deliberately not an io_context worker pool (§9), and the completion executor reaches it
# type-erased, so this layer links no asio.
add_subdirectory(atlas/db)
# architecture-design.md §10.2 — the cache client. Added after atlas/net because it links it for the
# asio aliases (net_types.h); unlike atlas/db it has no thread pool, because boost-redis is
# asio-native and never blocks the thread it runs on. That asymmetry is the point of §10.2.
add_subdirectory(atlas/redis)
add_subdirectory(generated)
# architecture-design.md §14 — the game layer, and §15.4 is why it is not a subdirectory of atlas/:
# the core-purity gate fails the build if `item` / `inventory` / `equip` appear under server/atlas/**.
# It is added last because it links everything above it and nothing above it links back.
add_subdirectory(game)
# architecture-design.md §16.1 — the load harness. A client of the game above, so it is added after
# it; nothing links back to it.
add_subdirectory(loadgen)
# architecture-design.md §15.3 — the interactive console client. Same shape as the harness above: a
# client of the game, added after it, and nothing links back to it. server/Dockerfile must COPY
# this directory for the same reason it copies loadgen — the builder configures this CMakeLists and
# a missing subdirectory fails at configure time, not at build time.
add_subdirectory(console_client)
add_subdirectory(tests)
