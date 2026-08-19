# architecture-design.md §15.2 — the MySQL client is libmariadb (2026-08-10 replacement). MariaDB
# Connector/C speaks both the MySQL wire protocol and the MySQL C API (mysql.h), so the server this
# connects to is still MySQL; only the client library changed.
find_package(unofficial-libmariadb CONFIG REQUIRED)

add_library(atlas_db STATIC
    connection.cpp
    connection_pool.cpp
    db_runner.cpp
    idempotency.cpp
    prepared_statement.cpp
    transaction.cpp)

# atlas_core carries cxx_std_20 / NOMINMAX / warning level and the server/ include root (§7.2).
# PUBLIC because connection.h exposes DbValue (built on core/types.h and core/time.h) and DbException
# (core/error.h), so every consumer needs the same include root and the same standard.
target_link_libraries(atlas_db PUBLIC atlas_core)

# PRIVATE is the mechanical enforcement of "no header in this library includes <mysql.h>". The
# driver handles appear in the public headers only as forward-declared struct tags, and a PUBLIC link
# would compile today while quietly permitting a MYSQL_BIND to leak into a public header tomorrow.
# Same shape as spdlog behind core/log.h (§11.1).
target_link_libraries(atlas_db PRIVATE unofficial::libmariadb)
