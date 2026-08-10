#pragma once
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atlas/core/types.h"
#include "atlas/db/connection.h"

// 🔴 architecture-design.md §10 — no SQL is assembled at run time. A statement is fixed text with
// `?` placeholders, prepared once and executed with bound parameters forever after. The generated
// headers (§10.1) emit exactly that text plus the binding order, and this class is what runs it.
//
// The driver's statement handle, named by its own C API struct tag for the reason connection.h
// gives. <mysql.h> stays out of every header in this library.
struct st_mysql_stmt;

namespace atlas {

// One prepared statement, owned by the Connection it was prepared on and cached there by SQL text.
//
// 🔴 Lifetime: a statement never outlives its connection. Connection's destructor tears the cache
// down first for that reason.
class PreparedStatement {
public:
    // Prepares immediately; throws DbException when the server rejects the text. Constructed by
    // Connection::Prepare, which is the only caller that has the driver handle.
    PreparedStatement(st_mysql* connection, std::string sql);
    ~PreparedStatement();

    PreparedStatement(const PreparedStatement&) = delete;
    PreparedStatement& operator=(const PreparedStatement&) = delete;
    PreparedStatement(PreparedStatement&&) = delete;
    PreparedStatement& operator=(PreparedStatement&&) = delete;

    // Number of `?` placeholders the server reported. The generated headers static_assert the same
    // number against their binding array, so a mismatch here means the statement was not the
    // generated one.
    [[nodiscard]] std::size_t ParameterCount() const noexcept { return parameter_count_; }
    [[nodiscard]] std::string_view Sql() const noexcept { return sql_; }

    // Runs a statement that returns no rows. Result: affected row count.
    UInt64 Execute(std::span<const DbValue> parameters);

    // Runs a statement that returns rows and materialises the whole result set.
    //
    // 🔴 Materialised on purpose: a streaming cursor would hold the connection past the end of the
    // caller's frame, and the connection belongs to a pool lease. Phase-1 result sets are a handful
    // of rows for one character, so the copy is not the cost that matters.
    std::vector<DbRow> Query(std::span<const DbValue> parameters);

private:
    void BindAndExecute(std::span<const DbValue> parameters);

    std::string sql_;
    st_mysql_stmt* stmt_{nullptr};
    std::size_t parameter_count_{0};
};

}  // namespace atlas
