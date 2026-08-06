find_package(GTest CONFIG REQUIRED)

add_executable(atlas_core_tests core_logging_test.cpp core_types_test.cpp)
target_link_libraries(atlas_core_tests PRIVATE atlas_core GTest::gtest GTest::gtest_main)

# A separate executable, not another source on atlas_core_tests: LogCount and the log level are
# process-wide, and the net tests assert on the ERROR count that Guarded produces. Sharing a process
# with the tests that deliberately move the level around would make that assertion order-dependent.
add_executable(atlas_net_tests net_session_test.cpp)
target_link_libraries(atlas_net_tests PRIVATE atlas_net GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(atlas_core_tests)
gtest_discover_tests(atlas_net_tests)
