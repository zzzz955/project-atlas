# The load harness (architecture-design.md §16.1 — the first load measurement).
#
# 🔴 It links atlas_demo, the game library, for the same reason the end-to-end test does: the wire
# it speaks is THIS game's opcode table and payload order, and a harness with its own copy would
# drift from the server it is measuring. What it must never become is something the server links
# back — the edge points loadgen -> game -> core and never returns (§14).
#
# 🔴 It is a client, so it is deliberately NOT part of the deployed runtime stage of
# server/Dockerfile. The image builds it (the builder stage builds every target) and leaves it
# there; shipping a load generator inside the server image would be a tool for an attacker.
add_executable(atlas_loadgen live_view.cpp load_client.cpp main.cpp)
target_link_libraries(atlas_loadgen PRIVATE atlas_demo)

# 🔴 Same libmariadb plugin-directory problem the test targets have (see tests/CMakeLists.txt):
# vcpkg bakes in the absolute path of its own build tree, which does not survive installation, and
# a client that cannot find caching_sha2_password fails the handshake while the SERVER reports
# "1045 access denied". The harness seeds its own rows, so it is a database client too.
set(ATLAS_MARIADB_PLUGIN_DIR
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/plugins/libmariadb")
if(EXISTS "${ATLAS_MARIADB_PLUGIN_DIR}")
    target_compile_definitions(atlas_loadgen PRIVATE
        ATLAS_MARIADB_PLUGIN_DIR="${ATLAS_MARIADB_PLUGIN_DIR}")
endif()
