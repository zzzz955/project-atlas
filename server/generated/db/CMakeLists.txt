# Owned by the db_generator work item.
#
# Every .h / .cpp / .cmake / .sql next to this file is generator output and is never hand-edited —
#    change server/db/schema.json and re-run `npm run gen:db`. This CMakeLists.txt and the tests/
#    directory are the only hand-written things here.
#
# The source list itself is generated into db_sources.cmake instead of being spelled out or
#    file(GLOB)-ed: adding a table then never means touching CMake, and a stale list can never
#    silently drop a row struct from the build. include() registers it as a configure dependency, so
#    regenerating re-runs CMake on its own.
include(${CMAKE_CURRENT_SOURCE_DIR}/db_sources.cmake)

add_library(atlas_generated_db STATIC ${ATLAS_GENERATED_DB_SOURCES})

# cpp-style.md §7.2 — atlas_core propagates the include root (server/), C++20, NOMINMAX /
# WIN32_LEAN_AND_MEAN and the warning level. PUBLIC because the generated headers include
# "atlas/core/types.h", "atlas/core/ids.h" and "atlas/core/time.h", so anything using these row
# structs needs the same include root.
target_link_libraries(atlas_generated_db PUBLIC atlas_core)

add_subdirectory(tests)
