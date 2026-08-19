# architecture-design.md §10.2 — the cache client. `boost-redis` (§15.2), asio-native, so it runs on
# the io_context this server already has instead of on a pool of its own.
find_package(boost_redis CONFIG REQUIRED)

add_library(atlas_redis STATIC
    connection.cpp
    redis_runner.cpp
    src.cpp)

# src.cpp is boost-redis's own implementation aggregate, not project code. Keeping it in its own
# translation unit is why the unity ON and unity OFF builds see the same thing here, and it stops an
# external library's internals from sharing an anonymous namespace with ours (§15.1).
set_source_files_properties(src.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

# PUBLIC atlas_net, and the reason is narrow: connection.h names IoContext, which is one of the asio
# aliases cpp-style.md §4.2 fixed in atlas/net/net_types.h. Spelling them a second time here would be
# exactly the alias sprawl that section forbids. No socket, acceptor or session of that library is
# used — the dependency is on the alias header, and the direction (redis → net) never reverses.
target_link_libraries(atlas_redis PUBLIC atlas_net)

# PRIVATE is the mechanical enforcement of "no header in this library includes <boost/redis.hpp>".
# The driver lives behind a pimpl, and a PUBLIC link would compile today while quietly permitting a
# boost::redis type to surface in a public header tomorrow — the same shape atlas_db uses for
# libmariadb and core/log.h uses for spdlog (§11.1).
target_link_libraries(atlas_redis PRIVATE Boost::redis)

# OpenSSL is named explicitly, and the reason is a link failure only Linux produces.
# `Boost::redis` does not carry OpenSSL in its usage requirements, so on GNU ld nothing put
# libssl/libcrypto after libatlas_redis.a and src.cpp.o came out with `undefined reference to
# BIO_new_bio_pair`. MSVC resolves static archives without caring about order, which is why the
# Windows gate stayed green while the container image and linux-ci could not link at all
# (2026-08-11, found by building server/Dockerfile). PRIVATE is still correct - no header here
# names an OpenSSL type - and for a STATIC library CMake still forwards it to the final link.
find_package(OpenSSL REQUIRED)
target_link_libraries(atlas_redis PRIVATE OpenSSL::SSL OpenSSL::Crypto)
