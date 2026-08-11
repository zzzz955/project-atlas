// The one translation unit that compiles boost-redis itself.
//
// 🔴 boost-redis ships as headers plus a single implementation aggregate, and vcpkg installs it as
// an INTERFACE target with no library to link. Without this file the build compiles cleanly and
// then fails at LINK time with nine unresolved symbols, which points nowhere near the cause.
//
// 🔴 It is kept out of the unity batch (see CMakeLists.txt): the library's own sources are not this
// project's sources, and merging them into a batch with atlas code would put an external
// implementation inside the same translation unit as ours for no benefit.
#include <boost/redis/src.hpp>
