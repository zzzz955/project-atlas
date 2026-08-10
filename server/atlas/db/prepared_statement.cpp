#include "atlas/db/prepared_statement.h"

#include <mysql.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "atlas/core/time.h"
#include "atlas/core/types.h"

namespace atlas {
namespace {

// 🔴 Derived from the driver struct instead of being spelled out. That field's C type is the
// platform-dependent wide unsigned one — 4 bytes on Windows (LLP64), 8 on Linux (LP64) — which is
// exactly the drift cpp-style.md §4.1 bans us from writing by hand, and the reason a grep for the
// banned spellings over this directory has to come back empty. Taking the type from the struct is
// correct on both platforms and stays correct if the driver ever changes it.
using DriverLength = std::remove_pointer_t<decltype(MYSQL_BIND::length)>;

// How a result column is read back. Derived from the field type the server reports, so the caller
// never has to describe its own result set — which is what keeps this layer generic and free of any
// knowledge of a particular table.
enum class ColumnKind : UInt8 { Signed, Unsigned, Real, Text, Time };

// Frees the result metadata on every exit path, including a throw out of the fetch loop.
class ResultMetadata {
public:
    explicit ResultMetadata(MYSQL_RES* handle) noexcept : handle_(handle) {}
    ~ResultMetadata() {
        if (handle_ != nullptr) {
            mysql_free_result(handle_);
        }
    }

    ResultMetadata(const ResultMetadata&) = delete;
    ResultMetadata& operator=(const ResultMetadata&) = delete;
    ResultMetadata(ResultMetadata&&) = delete;
    ResultMetadata& operator=(ResultMetadata&&) = delete;

    [[nodiscard]] MYSQL_RES* Get() const noexcept { return handle_; }

private:
    MYSQL_RES* handle_;
};

MYSQL_TIME ToDriverTime(SysTime value) {
    const std::chrono::sys_days day = std::chrono::floor<std::chrono::days>(value);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss<Micros> clock_time{std::chrono::floor<Micros>(value - day)};

    MYSQL_TIME out{};
    out.year = static_cast<UInt32>(static_cast<Int32>(date.year()));
    out.month = static_cast<UInt32>(date.month());
    out.day = static_cast<UInt32>(date.day());
    out.hour = static_cast<UInt32>(clock_time.hours().count());
    out.minute = static_cast<UInt32>(clock_time.minutes().count());
    out.second = static_cast<UInt32>(clock_time.seconds().count());
    // schema.sql declares DATETIME with no fractional seconds, so sending one would be rounded away
    // by the server and make a written value differ from the value read back.
    out.second_part = 0;
    out.time_type = MYSQL_TIMESTAMP_DATETIME;
    return out;
}

SysTime FromDriverTime(const MYSQL_TIME& value) {
    const std::chrono::year_month_day date{std::chrono::year{static_cast<Int32>(value.year)},
                                           std::chrono::month{value.month},
                                           std::chrono::day{value.day}};
    return std::chrono::sys_days{date} + std::chrono::hours{value.hour} +
           std::chrono::minutes{value.minute} + std::chrono::seconds{value.second} +
           Micros{static_cast<Int64>(value.second_part)};
}

ColumnKind KindOf(const MYSQL_FIELD& field) {
    const bool is_unsigned = (field.flags & UNSIGNED_FLAG) != 0;
    switch (field.type) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_YEAR:
            return is_unsigned ? ColumnKind::Unsigned : ColumnKind::Signed;
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
            return ColumnKind::Real;
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIMESTAMP:
            return ColumnKind::Time;
        default:
            // Text covers VARCHAR / CHAR / BLOB / JSON and also DECIMAL, which the server sends as
            // a string precisely so that no client can round it through a binary float.
            return ColumnKind::Text;
    }
}

// Scratch the MYSQL_BIND array points into. It has to stay alive until mysql_stmt_execute returns,
// so it is a local of the calling frame and never a member: a member would be a re-entrancy trap
// the moment one statement was executed from inside another's result loop.
struct ParameterScratch {
    std::vector<MYSQL_BIND> binds;
    std::vector<my_bool> nulls;
    std::vector<DriverLength> lengths;
    std::vector<MYSQL_TIME> times;
};

void BuildParameters(std::span<const DbValue> parameters, ParameterScratch& scratch) {
    const std::size_t count = parameters.size();
    scratch.binds.assign(count, MYSQL_BIND{});
    scratch.nulls.assign(count, 0);
    scratch.lengths.assign(count, 0);
    scratch.times.assign(count, MYSQL_TIME{});

    for (std::size_t index = 0; index < count; ++index) {
        MYSQL_BIND& bind = scratch.binds[index];
        std::visit(
            [&](const auto& value) {
                using Held = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Held, std::monostate>) {
                    bind.buffer_type = MYSQL_TYPE_NULL;
                    scratch.nulls[index] = 1;
                    bind.is_null = &scratch.nulls[index];
                } else if constexpr (std::is_same_v<Held, Int64>) {
                    bind.buffer_type = MYSQL_TYPE_LONGLONG;
                    bind.buffer = const_cast<Int64*>(&value);
                    bind.is_unsigned = 0;
                } else if constexpr (std::is_same_v<Held, UInt64>) {
                    bind.buffer_type = MYSQL_TYPE_LONGLONG;
                    bind.buffer = const_cast<UInt64*>(&value);
                    bind.is_unsigned = 1;
                } else if constexpr (std::is_same_v<Held, Float64>) {
                    bind.buffer_type = MYSQL_TYPE_DOUBLE;
                    bind.buffer = const_cast<Float64*>(&value);
                } else if constexpr (std::is_same_v<Held, std::string>) {
                    // Points straight into the caller's string. `parameters` outlives the execute
                    // call by construction — it is a span over the caller's own storage — so there
                    // is nothing to copy here.
                    scratch.lengths[index] = static_cast<DriverLength>(value.size());
                    bind.buffer_type = MYSQL_TYPE_STRING;
                    bind.buffer = const_cast<char*>(value.data());
                    bind.buffer_length = scratch.lengths[index];
                    bind.length = &scratch.lengths[index];
                } else {
                    scratch.times[index] = ToDriverTime(value);
                    bind.buffer_type = MYSQL_TYPE_DATETIME;
                    bind.buffer = &scratch.times[index];
                }
            },
            parameters[index]);
    }
}

}  // namespace

PreparedStatement::PreparedStatement(st_mysql* connection, std::string sql) : sql_(std::move(sql)) {
    stmt_ = mysql_stmt_init(connection);
    if (stmt_ == nullptr) {
        ATLAS_THROW(DbException, "mysql_stmt_init failed");
    }

    if (mysql_stmt_prepare(stmt_, sql_.data(), static_cast<DriverLength>(sql_.size())) != 0) {
        const std::string reason = mysql_stmt_error(stmt_);
        mysql_stmt_close(stmt_);
        stmt_ = nullptr;
        // The statement text is safe to report: it is a fixed constant compiled into the binary,
        // never a value. That is the whole difference between this and the connect path.
        ATLAS_THROW(DbException, "db prepare failed [{}]: {}", sql_, reason);
    }

    parameter_count_ = mysql_stmt_param_count(stmt_);
}

PreparedStatement::~PreparedStatement() {
    if (stmt_ != nullptr) {
        mysql_stmt_close(stmt_);
        stmt_ = nullptr;
    }
}

void PreparedStatement::BindAndExecute(std::span<const DbValue> parameters) {
    if (parameters.size() != parameter_count_) {
        ATLAS_THROW(DbException, "db bind arity mismatch [{}]: statement takes {}, got {}", sql_,
                    parameter_count_, parameters.size());
    }

    // Releases whatever a previous Query left buffered on this statement. Skipping it makes the
    // second execute of a cached statement fail with "commands out of sync", which reads like a
    // threading bug and is not one.
    mysql_stmt_free_result(stmt_);

    ParameterScratch scratch;
    if (!parameters.empty()) {
        BuildParameters(parameters, scratch);
        if (mysql_stmt_bind_param(stmt_, scratch.binds.data()) != 0) {
            ATLAS_THROW(DbException, "db bind failed [{}]: {}", sql_, mysql_stmt_error(stmt_));
        }
    }

    if (mysql_stmt_execute(stmt_) != 0) {
        ATLAS_THROW(DbException, "db execute failed [{}]: {}", sql_, mysql_stmt_error(stmt_));
    }
}

UInt64 PreparedStatement::Execute(std::span<const DbValue> parameters) {
    BindAndExecute(parameters);
    return static_cast<UInt64>(mysql_stmt_affected_rows(stmt_));
}

std::vector<DbRow> PreparedStatement::Query(std::span<const DbValue> parameters) {
    BindAndExecute(parameters);

    const ResultMetadata metadata{mysql_stmt_result_metadata(stmt_)};
    if (metadata.Get() == nullptr) {
        // The statement produces no result set at all. Not an error — Query on an INSERT is simply
        // an empty answer.
        return {};
    }

    const UInt32 column_count = mysql_num_fields(metadata.Get());
    const MYSQL_FIELD* fields = mysql_fetch_fields(metadata.Get());

    if (mysql_stmt_store_result(stmt_) != 0) {
        ATLAS_THROW(DbException, "db store result failed [{}]: {}", sql_, mysql_stmt_error(stmt_));
    }

    std::vector<MYSQL_BIND> binds(column_count, MYSQL_BIND{});
    std::vector<my_bool> nulls(column_count, 0);
    std::vector<my_bool> errors(column_count, 0);
    std::vector<DriverLength> lengths(column_count, 0);
    std::vector<Int64> signed_values(column_count, 0);
    std::vector<UInt64> unsigned_values(column_count, 0);
    std::vector<Float64> real_values(column_count, 0.0);
    std::vector<MYSQL_TIME> time_values(column_count, MYSQL_TIME{});
    std::vector<ColumnKind> kinds(column_count, ColumnKind::Text);

    for (UInt32 column = 0; column < column_count; ++column) {
        kinds[column] = KindOf(fields[column]);
        MYSQL_BIND& bind = binds[column];
        bind.is_null = &nulls[column];
        bind.length = &lengths[column];
        bind.error = &errors[column];

        switch (kinds[column]) {
            case ColumnKind::Signed:
                bind.buffer_type = MYSQL_TYPE_LONGLONG;
                bind.buffer = &signed_values[column];
                bind.is_unsigned = 0;
                break;
            case ColumnKind::Unsigned:
                bind.buffer_type = MYSQL_TYPE_LONGLONG;
                bind.buffer = &unsigned_values[column];
                bind.is_unsigned = 1;
                break;
            case ColumnKind::Real:
                bind.buffer_type = MYSQL_TYPE_DOUBLE;
                bind.buffer = &real_values[column];
                break;
            case ColumnKind::Time:
                bind.buffer_type = MYSQL_TYPE_DATETIME;
                bind.buffer = &time_values[column];
                break;
            case ColumnKind::Text:
                // 🔴 Zero-length buffer on purpose. A text column has no width known before the
                // row arrives, and sizing off the DECLARED width would allocate 4 GB for a
                // LONGTEXT. The driver reports the real length in lengths[column] and the row loop
                // then pulls the bytes with mysql_stmt_fetch_column.
                bind.buffer_type = MYSQL_TYPE_STRING;
                bind.buffer = nullptr;
                bind.buffer_length = 0;
                break;
        }
    }

    if (mysql_stmt_bind_result(stmt_, binds.data()) != 0) {
        ATLAS_THROW(DbException, "db bind result failed [{}]: {}", sql_, mysql_stmt_error(stmt_));
    }

    std::vector<DbRow> rows;
    while (true) {
        const Int32 status = static_cast<Int32>(mysql_stmt_fetch(stmt_));
        if (status == MYSQL_NO_DATA) {
            break;
        }
        // MYSQL_DATA_TRUNCATED is the expected answer, not a failure: every text column above was
        // bound with a zero-length buffer precisely so the driver would report its length here.
        if (status != 0 && status != MYSQL_DATA_TRUNCATED) {
            ATLAS_THROW(DbException, "db fetch failed [{}]: {}", sql_, mysql_stmt_error(stmt_));
        }

        DbRow row(column_count);
        for (UInt32 column = 0; column < column_count; ++column) {
            if (nulls[column] != 0) {
                row[column] = std::monostate{};
                continue;
            }
            switch (kinds[column]) {
                case ColumnKind::Signed:
                    row[column] = signed_values[column];
                    break;
                case ColumnKind::Unsigned:
                    row[column] = unsigned_values[column];
                    break;
                case ColumnKind::Real:
                    row[column] = real_values[column];
                    break;
                case ColumnKind::Time:
                    row[column] = FromDriverTime(time_values[column]);
                    break;
                case ColumnKind::Text: {
                    std::string text(lengths[column], '\0');
                    if (!text.empty()) {
                        MYSQL_BIND text_bind{};
                        DriverLength written = 0;
                        text_bind.buffer_type = MYSQL_TYPE_STRING;
                        text_bind.buffer = text.data();
                        text_bind.buffer_length = lengths[column];
                        text_bind.length = &written;
                        if (mysql_stmt_fetch_column(stmt_, &text_bind, column, 0) != 0) {
                            ATLAS_THROW(DbException, "db fetch column failed [{}]: {}", sql_,
                                        mysql_stmt_error(stmt_));
                        }
                    }
                    row[column] = std::move(text);
                    break;
                }
            }
        }
        rows.push_back(std::move(row));
    }

    mysql_stmt_free_result(stmt_);
    return rows;
}

}  // namespace atlas
