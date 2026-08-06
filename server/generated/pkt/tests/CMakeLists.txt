# Tests for the pkt_generator C++ target. 🔴 Hand-written, unlike everything one level up.
# They live under generated/pkt/ (not server/tests/) so this work item owns a single directory
# and cannot collide with the core test file.
find_package(GTest CONFIG REQUIRED)

add_executable(atlas_generated_pkt_tests pkt_codec_test.cpp)
target_link_libraries(atlas_generated_pkt_tests
    PRIVATE atlas_generated_pkt GTest::gtest GTest::gtest_main)

include(GoogleTest)
# TEST_PREFIX makes every case start with `pkt.` so `ctest -R pkt` selects the whole suite —
# ctest -R is case sensitive, and the GoogleTest suite names are PascalCase.
gtest_discover_tests(atlas_generated_pkt_tests TEST_PREFIX pkt.)

# ── The generator itself is under test too ───────────────────────────────────────────────────
# 🔴 A contract carrying a floating point field must make pkt_generator die loudly
# (architecture-design.md §8.4). If it silently emitted the field instead, the quantization rule
# would only fail visibly under a load test, well after the contract shipped. The fixture lives
# outside shared/contracts/ so a normal `npm run gen:pkt` never sees it.
set(ATLAS_REPO_ROOT ${CMAKE_SOURCE_DIR}/..)
find_program(ATLAS_NODE_EXECUTABLE node)
if(ATLAS_NODE_EXECUTABLE)
    set(ATLAS_PKT_FP_REJECT_COMMAND
        ${ATLAS_NODE_EXECUTABLE} ${ATLAS_REPO_ROOT}/tools/pkt_generator/pkt_generator.js
        --contracts-dir=${ATLAS_REPO_ROOT}/tools/pkt_generator/testdata/fp_reject
        --cpp-output-dir=${CMAKE_CURRENT_BINARY_DIR}/fp_reject_out)

    # Two assertions, because CTest cannot express both at once: PASS_REGULAR_EXPRESSION makes it
    # ignore the exit status, so the exit status needs its own test.
    add_test(NAME pkt.generator_rejects_fp_exit COMMAND ${ATLAS_PKT_FP_REJECT_COMMAND})
    set_tests_properties(pkt.generator_rejects_fp_exit PROPERTIES WILL_FAIL TRUE)

    add_test(NAME pkt.generator_rejects_fp_message COMMAND ${ATLAS_PKT_FP_REJECT_COMMAND})
    set_tests_properties(pkt.generator_rejects_fp_message PROPERTIES
        PASS_REGULAR_EXPRESSION "architecture-design.md 8\\.4")
else()
    # 🔴 Announced, never silent: a skipped gate that says nothing is a dead gate.
    message(WARNING
        "node not found - the pkt_generator floating-point rejection tests are NOT registered. "
        "node is required by this repository anyway (npm run gen:*), so this is a broken setup.")
endif()
