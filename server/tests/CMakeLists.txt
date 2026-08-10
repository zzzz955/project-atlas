find_package(GTest CONFIG REQUIRED)

add_executable(atlas_core_tests core_logging_test.cpp core_types_test.cpp)
target_link_libraries(atlas_core_tests PRIVATE atlas_core GTest::gtest GTest::gtest_main)

# A separate executable, not another source on atlas_core_tests: LogCount and the log level are
# process-wide, and the net tests assert on the ERROR count that Guarded produces. Sharing a process
# with the tests that deliberately move the level around would make that assertion order-dependent.
add_executable(atlas_net_tests net_session_test.cpp)
target_link_libraries(atlas_net_tests PRIVATE atlas_net GTest::gtest GTest::gtest_main)

# Its own executable for the same reason as the net suite: this one asserts on session close counts
# and on the WARN that a framing failure emits, and sharing a process with a suite that moves the log
# level around would make those observations order-dependent.
add_executable(atlas_proto_tests proto_frame_test.cpp)
target_link_libraries(atlas_proto_tests PRIVATE atlas_proto GTest::gtest GTest::gtest_main)

add_executable(atlas_config_tests config_test.cpp)
target_link_libraries(atlas_config_tests PRIVATE atlas_config GTest::gtest GTest::gtest_main)
# 🔴 The committed server.ini is located by an absolute path baked in at configure time. Deriving it
# from the working directory would make the test pass under ctest and fail everywhere else, and the
# ini is the file this suite exists to pin (architecture-design.md §5.4).
target_compile_definitions(atlas_config_tests PRIVATE
    ATLAS_SERVER_INI_PATH="${PROJECT_SOURCE_DIR}/server.ini")

# architecture-design.md §10.1 — the ORM runtime suite. It links the generated row headers as well
# as atlas_db: atlas/db itself may not include them (core-purity, §15.4), so the meeting point of
# the generated statements and the runtime that executes them is here, above the core. atlas_net is
# linked for the one case that asserts a completion lands back on the originating strand (§9).
add_executable(atlas_db_tests db_runtime_test.cpp)
target_link_libraries(atlas_db_tests
    PRIVATE atlas_db atlas_net atlas_config atlas_generated_db GTest::gtest GTest::gtest_main)

# architecture-design.md §10.4 — the fault-injection suite for atomicity across a server boundary.
#
# Its own executable rather than a second source on atlas_db_tests: with unity build ON the sources
# of one target become one translation unit, and the two suites' file-local helpers (a config
# loader, a fixture, a test server id) would then share a single anonymous namespace and collide.
# Two targets are two batches, so the isolation is structural instead of a naming convention.
add_executable(atlas_tx_tests tx_atomicity_test.cpp)
target_link_libraries(atlas_tx_tests
    PRIVATE atlas_db atlas_config atlas_generated_db GTest::gtest GTest::gtest_main)

# architecture-design.md §3.3 / §8.2 / §10 — the demo game: the domain model, the atomic equip, and
# the end-to-end socket run (load -> equip -> reconnect).
#
# 🔴 It links atlas_demo, the game library, NOT the atlas_game executable: a target with main() in it
# cannot be linked into a test. That split is the reason the server wiring lives in game/handlers.cpp
# and main.cpp is only a process shell — otherwise the server under test would be a second, parallel
# implementation of the one that ships.
#
# Its own executable for the same reason atlas_tx_tests is: with unity build ON the sources of one
# target become one translation unit, so two suites' file-local helpers (a config loader, a fixture,
# a test server id) would share a single anonymous namespace and collide.
add_executable(atlas_game_tests game_equip_test.cpp)
target_link_libraries(atlas_game_tests PRIVATE atlas_demo GTest::gtest GTest::gtest_main)

# 🔴 vcpkg compiles libmariadb's plugin directory in as an absolute path into its own build tree,
# which does not exist once the port is installed. MySQL 8.4 authenticates with
# caching_sha2_password and this driver ships that as a separate plugin DLL, so a client that cannot
# find the directory fails the handshake — and the server answers "1045 access denied", which points
# the reader at the credentials and never mentions a plugin. Measured 2026-08-10: the suite skipped
# against a healthy `docker compose up mysql` until this was passed through.
# Guarded by EXISTS so that a layout without this directory keeps the driver's own default rather
# than being handed a path that is not there.
set(ATLAS_MARIADB_PLUGIN_DIR
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/plugins/libmariadb")
if(EXISTS "${ATLAS_MARIADB_PLUGIN_DIR}")
    target_compile_definitions(atlas_db_tests PRIVATE
        ATLAS_MARIADB_PLUGIN_DIR="${ATLAS_MARIADB_PLUGIN_DIR}")
    target_compile_definitions(atlas_tx_tests PRIVATE
        ATLAS_MARIADB_PLUGIN_DIR="${ATLAS_MARIADB_PLUGIN_DIR}")
    target_compile_definitions(atlas_game_tests PRIVATE
        ATLAS_MARIADB_PLUGIN_DIR="${ATLAS_MARIADB_PLUGIN_DIR}")
endif()

include(GoogleTest)
gtest_discover_tests(atlas_core_tests)
gtest_discover_tests(atlas_net_tests)
gtest_discover_tests(atlas_proto_tests)
gtest_discover_tests(atlas_config_tests)

# 🔴 The DB suite needs a real MySQL, and with none reachable it must be visibly skipped rather than
# quietly green — a suite that "passes" without a database retires the question it exists to ask.
# GTEST_SKIP() alone still exits 0, which ctest reports as Passed; this property is what turns it
# into ctest's own Skipped state, so the run summary shows the gap.
gtest_discover_tests(atlas_db_tests PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED: ")

# Same rule for the fault-injection suite. Its four injection cases need a real database; the
# ordering and TTL cases below them do not, and those run everywhere.
gtest_discover_tests(atlas_tx_tests PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED: ")

# Same rule again for the game suite: the equip cases and the end-to-end socket run need a real
# database, the domain-model cases do not.
gtest_discover_tests(atlas_game_tests PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED: ")
