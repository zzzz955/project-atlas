# Tests for the db_generator C++ target. Hand-written, unlike everything one level up.
# They live under generated/db/ (not server/tests/) so this work item owns a single directory and
# cannot collide with the core test file or with the pkt tests.
find_package(GTest CONFIG REQUIRED)

add_executable(atlas_generated_db_tests db_schema_test.cpp)
target_link_libraries(atlas_generated_db_tests
    PRIVATE atlas_generated_db GTest::gtest GTest::gtest_main)

include(GoogleTest)
# TEST_PREFIX makes every case start with `db.` so `ctest -R db` selects the whole suite —
# ctest -R is case sensitive, and the GoogleTest suite names are PascalCase.
gtest_discover_tests(atlas_generated_db_tests TEST_PREFIX db.)

# ── The generator itself is under test too ───────────────────────────────────────────────────
# A schema that stores an identifier narrower than its strong-typed ID must make db_generator die
# loudly (cpp-style.md §4.3). Emitting it anyway would truncate every identifier above 2^32 — and
# identifiers are handed out by the server, so the damage would only surface in production data,
# well after the schema shipped. The fixture lives outside server/db/ so a normal `npm run gen:db`
# never sees it, and it writes into the build tree so it cannot touch server/generated/db/.
set(ATLAS_REPO_ROOT ${CMAKE_SOURCE_DIR}/..)
find_program(ATLAS_NODE_EXECUTABLE node)
if(ATLAS_NODE_EXECUTABLE)
    set(ATLAS_DB_ID_WIDTH_COMMAND
        ${ATLAS_NODE_EXECUTABLE} ${ATLAS_REPO_ROOT}/tools/db_generator/db_generator.js
        --schema=${ATLAS_REPO_ROOT}/tools/db_generator/testdata/id_width_mismatch/schema.json
        --cpp-output-dir=${CMAKE_CURRENT_BINARY_DIR}/id_width_out
        --sql-output=${CMAKE_CURRENT_BINARY_DIR}/id_width_out/schema.sql)

    # Two assertions, because CTest cannot express both at once: PASS_REGULAR_EXPRESSION makes it
    # ignore the exit status, so the exit status needs its own test.
    add_test(NAME db.generator_rejects_narrow_id_exit COMMAND ${ATLAS_DB_ID_WIDTH_COMMAND})
    set_tests_properties(db.generator_rejects_narrow_id_exit PROPERTIES WILL_FAIL TRUE)

    add_test(NAME db.generator_rejects_narrow_id_message COMMAND ${ATLAS_DB_ID_WIDTH_COMMAND})
    set_tests_properties(db.generator_rejects_narrow_id_message PROPERTIES
        PASS_REGULAR_EXPRESSION "cpp-style.md 4\\.3")
else()
    # Announced, never silent: a skipped gate that says nothing is a dead gate.
    message(WARNING
        "node not found - the db_generator identifier-width rejection tests are NOT registered. "
        "node is required by this repository anyway (npm run gen:*), so this is a broken setup.")
endif()
