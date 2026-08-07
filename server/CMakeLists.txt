# project-atlas — CMake root (architecture-design.md §15.1). CMake is the build SoT.
cmake_minimum_required(VERSION 3.25)

# 🔴 CMAKE_TOOLCHAIN_FILE (vcpkg) must be in the cache before project() runs — CMakePresets.json
# sets it. Do not set it here and do not hardcode a vcpkg path.
project(project-atlas CXX)

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
add_subdirectory(generated)
add_subdirectory(tests)
