# Owned by the info_generator work item.
#
# Every .h / .cpp / .cmake next to this file is generator output and is never hand-edited —
#    change the CSV under shared/datas/ and re-run `npm run gen:info`. This CMakeLists.txt is the
#    only hand-written thing here.
#
# The source list itself is generated into info_sources.cmake instead of being spelled out or
#    file(GLOB)-ed: adding a CSV then never means touching CMake, and a stale list can never
#    silently drop a table from the build. include() registers it as a configure dependency, so
#    regenerating re-runs CMake on its own.
include(${CMAKE_CURRENT_SOURCE_DIR}/info_sources.cmake)

add_library(atlas_generated_info STATIC ${ATLAS_GENERATED_INFO_SOURCES})

# cpp-style.md §7.2 — atlas_core propagates the include root (server/), C++20, NOMINMAX /
# WIN32_LEAN_AND_MEAN and the warning level. PUBLIC because the generated headers include
# "atlas/core/types.h", so anything using these tables needs the same include root.
target_link_libraries(atlas_generated_info PUBLIC atlas_core)

# No tests/ subdirectory, unlike generated/db/. The consumer of this table is the equip path, so
# its tests live next to the other game tests in server/tests/ — including the ones that drive the
# generator itself against the malformed-CSV fixtures under tools/info_generator/testdata/.
