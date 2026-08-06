find_package(boost_asio CONFIG REQUIRED)

# STATIC from here on: this slice adds the first compilable net sources, so the INTERFACE stopgap
# that the header-only slice needed (a STATIC target with no source has no link language) is gone.
add_library(atlas_net STATIC acceptor.cpp io_runner.cpp session.cpp)

# atlas_core carries cxx_std_20 / NOMINMAX / warning level and the server/ include root (§7.2).
# PUBLIC: net_types.h is a public header and includes <boost/asio.hpp>, so every consumer needs the
# include path too - a PRIVATE link would compile this library and break the first user of it.
target_link_libraries(atlas_net PUBLIC atlas_core Boost::asio)

# 🔴 Without this Boost.Asio prints "Assuming _WIN32_WINNT=0x0601 (Windows 7)" and then compiles
# itself against the Windows 7 API surface - a silently different socket implementation from the one
# prod (Linux) and this machine actually run. PUBLIC because net_types.h pulls <boost/asio.hpp> into
# every consumer, and a consumer that assumed a different value would be an ODR violation.
target_compile_definitions(atlas_net PUBLIC $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0A00>)

# architecture-design.md §15.1 - asio is the heavy header set of this project and it is stable, so
# it is exactly what a PCH is for. It lands here rather than on atlas_core because core deliberately
# does not depend on asio (cpp-style.md §4.2: types.h stays cheap).
target_precompile_headers(atlas_net PRIVATE <boost/asio.hpp>)
