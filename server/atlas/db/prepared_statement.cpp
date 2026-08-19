// =============================================================================
// PreparedStatement 구현. 파라미터 바인딩과 결과 행 물질화
// 컬럼 타입은 서버가 보고한 필드에서 유도하므로 테이블을 알지 않음
// =============================================================================

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

namespace atlas
{
namespace
{

// 직접 적지 않고 드라이버 구조체에서 유도
// 그 필드의 C 타입은 플랫폼 의존(Windows LLP64 4바이트, Linux LP64 8바이트)
// [CS 4.1] 이 손으로 쓰는 것을 금지한 바로 그 드리프트
using DriverLength = std::remove_pointer_t< decltype( MYSQL_BIND::length ) >;

// 결과 컬럼을 읽는 방식. 서버가 보고한 필드 타입에서 유도
// 호출자가 자기 결과 집합을 설명할 필요가 없음
// 그것이 이 계층을 테이블과 무관하게 유지함
enum class ColumnKind : UInt8
{
    Signed,
    Unsigned,
    Real,
    Text,
    Time
};

// fetch 루프 밖으로의 throw 를 포함해 모든 출구에서 결과 메타데이터를 해제
class ResultMetadata
{
public:
    explicit ResultMetadata( MYSQL_RES* handle ) noexcept : handle_( handle ) {}
    ~ResultMetadata()
    {
        if ( handle_ != nullptr )
        {
            mysql_free_result( handle_ );
        }
    }

    ResultMetadata( const ResultMetadata& ) = delete;
    ResultMetadata& operator=( const ResultMetadata& ) = delete;
    ResultMetadata( ResultMetadata&& ) = delete;
    ResultMetadata& operator=( ResultMetadata&& ) = delete;

    [[nodiscard]] MYSQL_RES* Get() const noexcept { return handle_; }

private:
    MYSQL_RES* handle_;
};

MYSQL_TIME ToDriverTime( SysTime value )
{
    const std::chrono::sys_days day = std::chrono::floor< std::chrono::days >( value );
    const std::chrono::year_month_day date{ day };
    const std::chrono::hh_mm_ss< Micros > clock_time{ std::chrono::floor< Micros >( value - day ) };

    MYSQL_TIME out{};
    out.year = static_cast< UInt32 >( static_cast< Int32 >( date.year() ) );
    out.month = static_cast< UInt32 >( date.month() );
    out.day = static_cast< UInt32 >( date.day() );
    out.hour = static_cast< UInt32 >( clock_time.hours().count() );
    out.minute = static_cast< UInt32 >( clock_time.minutes().count() );
    out.second = static_cast< UInt32 >( clock_time.seconds().count() );
    // schema.sql 의 DATETIME 에 소수초가 없음
    // 보내면 서버가 반올림해 쓴 값과 읽은 값이 달라짐
    out.second_part = 0;
    out.time_type = MYSQL_TIMESTAMP_DATETIME;
    return out;
}

SysTime FromDriverTime( const MYSQL_TIME& value )
{
    const std::chrono::year_month_day date{ std::chrono::year{ static_cast< Int32 >( value.year ) },
                                            std::chrono::month{ value.month },
                                            std::chrono::day{ value.day } };
    return std::chrono::sys_days{ date } + std::chrono::hours{ value.hour } +
           std::chrono::minutes{ value.minute } + std::chrono::seconds{ value.second } +
           Micros{ static_cast< Int64 >( value.second_part ) };
}

ColumnKind KindOf( const MYSQL_FIELD& field )
{
    const bool is_unsigned = ( field.flags & UNSIGNED_FLAG ) != 0;
    switch ( field.type )
    {
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
            // Text 는 VARCHAR / CHAR / BLOB / JSON 과 DECIMAL 까지 덮음
            // 서버가 DECIMAL 을 문자열로 보내는 이유는 이진 부동소수 왕복을 막으려는 것
            return ColumnKind::Text;
    }
}

// MYSQL_BIND 배열이 가리키는 스크래치
// mysql_stmt_execute 가 반환할 때까지 살아 있어야 해서 호출 프레임의 지역 변수
// 멤버였다면 다른 문장의 결과 루프 안에서 실행하는 순간 재진입 함정
struct ParameterScratch
{
    std::vector< MYSQL_BIND > binds;
    std::vector< my_bool > nulls;
    std::vector< DriverLength > lengths;
    std::vector< MYSQL_TIME > times;
};

void BuildParameters( std::span< const DbValue > parameters, ParameterScratch& scratch )
{
    const std::size_t count = parameters.size();
    scratch.binds.assign( count, MYSQL_BIND{} );
    scratch.nulls.assign( count, 0 );
    scratch.lengths.assign( count, 0 );
    scratch.times.assign( count, MYSQL_TIME{} );

    for ( std::size_t index = 0; index < count; ++index )
    {
        MYSQL_BIND& bind = scratch.binds[index];
        std::visit(
            [&]( const auto& value )
            {
                using Held = std::decay_t< decltype( value ) >;
                if constexpr ( std::is_same_v< Held, std::monostate > )
                {
                    bind.buffer_type = MYSQL_TYPE_NULL;
                    scratch.nulls[index] = 1;
                    bind.is_null = &scratch.nulls[index];
                }
                else if constexpr ( std::is_same_v< Held, Int64 > )
                {
                    bind.buffer_type = MYSQL_TYPE_LONGLONG;
                    bind.buffer = const_cast< Int64* >( &value );
                    bind.is_unsigned = 0;
                }
                else if constexpr ( std::is_same_v< Held, UInt64 > )
                {
                    bind.buffer_type = MYSQL_TYPE_LONGLONG;
                    bind.buffer = const_cast< UInt64* >( &value );
                    bind.is_unsigned = 1;
                }
                else if constexpr ( std::is_same_v< Held, Float64 > )
                {
                    bind.buffer_type = MYSQL_TYPE_DOUBLE;
                    bind.buffer = const_cast< Float64* >( &value );
                }
                else if constexpr ( std::is_same_v< Held, std::string > )
                {
                    // 호출자 문자열을 그대로 가리킴
                    // parameters 는 호출자 저장소 위의 span 이라 더 오래 삼
                    // 복사할 것이 없음
                    scratch.lengths[index] = static_cast< DriverLength >( value.size() );
                    bind.buffer_type = MYSQL_TYPE_STRING;
                    bind.buffer = const_cast< char* >( value.data() );
                    bind.buffer_length = scratch.lengths[index];
                    bind.length = &scratch.lengths[index];
                }
                else
                {
                    scratch.times[index] = ToDriverTime( value );
                    bind.buffer_type = MYSQL_TYPE_DATETIME;
                    bind.buffer = &scratch.times[index];
                }
            },
            parameters[index] );
    }
}

}  // namespace

PreparedStatement::PreparedStatement( st_mysql* connection, std::string sql )
    : sql_( std::move( sql ) )
{
    stmt_ = mysql_stmt_init( connection );
    if ( stmt_ == nullptr )
    {
        ATLAS_THROW( DbException, "mysql_stmt_init failed" );
    }

    if ( mysql_stmt_prepare( stmt_, sql_.data(), static_cast< DriverLength >( sql_.size() ) ) != 0 )
    {
        const std::string reason = mysql_stmt_error( stmt_ );
        mysql_stmt_close( stmt_ );
        stmt_ = nullptr;
        // 문장 텍스트는 보고해도 안전. 바이너리에 박힌 고정 상수이지 값이 아님
        // 접속 경로와 갈리는 지점이 정확히 그것
        ATLAS_THROW( DbException, "db prepare failed [{}]: {}", sql_, reason );
    }

    parameter_count_ = mysql_stmt_param_count( stmt_ );
}

PreparedStatement::~PreparedStatement()
{
    if ( stmt_ != nullptr )
    {
        mysql_stmt_close( stmt_ );
        stmt_ = nullptr;
    }
}

void PreparedStatement::BindAndExecute( std::span< const DbValue > parameters )
{
    if ( parameters.size() != parameter_count_ )
    {
        ATLAS_THROW( DbException, "db bind arity mismatch [{}]: statement takes {}, got {}", sql_,
                     parameter_count_, parameters.size() );
    }

    // 이전 Query 가 이 문장에 남긴 버퍼를 해제
    // 건너뛰면 캐시된 문장의 두 번째 실행이 "commands out of sync" 로 실패
    // 스레딩 버그처럼 읽히지만 아님
    mysql_stmt_free_result( stmt_ );

    ParameterScratch scratch;
    if ( !parameters.empty() )
    {
        BuildParameters( parameters, scratch );
        if ( mysql_stmt_bind_param( stmt_, scratch.binds.data() ) != 0 )
        {
            ATLAS_THROW( DbException, "db bind failed [{}]: {}", sql_, mysql_stmt_error( stmt_ ) );
        }
    }

    if ( mysql_stmt_execute( stmt_ ) != 0 )
    {
        ATLAS_THROW( DbException, "db execute failed [{}]: {}", sql_, mysql_stmt_error( stmt_ ) );
    }
}

UInt64 PreparedStatement::Execute( std::span< const DbValue > parameters )
{
    BindAndExecute( parameters );
    return static_cast< UInt64 >( mysql_stmt_affected_rows( stmt_ ) );
}

std::vector< DbRow > PreparedStatement::Query( std::span< const DbValue > parameters )
{
    BindAndExecute( parameters );

    const ResultMetadata metadata{ mysql_stmt_result_metadata( stmt_ ) };
    if ( metadata.Get() == nullptr )
    {
        // 결과 집합이 아예 없는 문장. 에러가 아니라 INSERT 에 Query 를 부른 것뿐
        return {};
    }

    const UInt32 column_count = mysql_num_fields( metadata.Get() );
    const MYSQL_FIELD* fields = mysql_fetch_fields( metadata.Get() );

    if ( mysql_stmt_store_result( stmt_ ) != 0 )
    {
        ATLAS_THROW( DbException, "db store result failed [{}]: {}", sql_,
                     mysql_stmt_error( stmt_ ) );
    }

    std::vector< MYSQL_BIND > binds( column_count, MYSQL_BIND{} );
    std::vector< my_bool > nulls( column_count, 0 );
    std::vector< my_bool > errors( column_count, 0 );
    std::vector< DriverLength > lengths( column_count, 0 );
    std::vector< Int64 > signed_values( column_count, 0 );
    std::vector< UInt64 > unsigned_values( column_count, 0 );
    std::vector< Float64 > real_values( column_count, 0.0 );
    std::vector< MYSQL_TIME > time_values( column_count, MYSQL_TIME{} );
    std::vector< ColumnKind > kinds( column_count, ColumnKind::Text );

    for ( UInt32 column = 0; column < column_count; ++column )
    {
        kinds[column] = KindOf( fields[column] );
        MYSQL_BIND& bind = binds[column];
        bind.is_null = &nulls[column];
        bind.length = &lengths[column];
        bind.error = &errors[column];

        switch ( kinds[column] )
        {
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
                // 길이 0 버퍼는 의도. 텍스트 컬럼은 행이 오기 전엔 폭을 모름
                // 선언 폭으로 잡으면 LONGTEXT 에 4GB 를 할당하게 됨
                // 실제 길이는 드라이버가 lengths[column] 에 알려줌
                bind.buffer_type = MYSQL_TYPE_STRING;
                bind.buffer = nullptr;
                bind.buffer_length = 0;
                break;
        }
    }

    if ( mysql_stmt_bind_result( stmt_, binds.data() ) != 0 )
    {
        ATLAS_THROW( DbException, "db bind result failed [{}]: {}", sql_,
                     mysql_stmt_error( stmt_ ) );
    }

    std::vector< DbRow > rows;
    while ( true )
    {
        const auto status = static_cast< Int32 >( mysql_stmt_fetch( stmt_ ) );
        if ( status == MYSQL_NO_DATA )
        {
            break;
        }
        // MYSQL_DATA_TRUNCATED 는 실패가 아니라 예상된 답
        // 위에서 텍스트 컬럼을 길이 0 으로 바인딩한 것이 여기서 길이를 받기 위해서
        if ( status != 0 && status != MYSQL_DATA_TRUNCATED )
        {
            ATLAS_THROW( DbException, "db fetch failed [{}]: {}", sql_, mysql_stmt_error( stmt_ ) );
        }

        DbRow row( column_count );
        for ( UInt32 column = 0; column < column_count; ++column )
        {
            if ( nulls[column] != 0 )
            {
                row[column] = std::monostate{};
                continue;
            }
            switch ( kinds[column] )
            {
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
                    row[column] = FromDriverTime( time_values[column] );
                    break;
                case ColumnKind::Text:
                {
                    std::string text( lengths[column], '\0' );
                    if ( !text.empty() )
                    {
                        MYSQL_BIND text_bind{};
                        DriverLength written = 0;
                        text_bind.buffer_type = MYSQL_TYPE_STRING;
                        text_bind.buffer = text.data();
                        text_bind.buffer_length = lengths[column];
                        text_bind.length = &written;
                        if ( mysql_stmt_fetch_column( stmt_, &text_bind, column, 0 ) != 0 )
                        {
                            ATLAS_THROW( DbException, "db fetch column failed [{}]: {}", sql_,
                                         mysql_stmt_error( stmt_ ) );
                        }
                    }
                    row[column] = std::move( text );
                    break;
                }
            }
        }
        rows.push_back( std::move( row ) );
    }

    mysql_stmt_free_result( stmt_ );
    return rows;
}

}  // namespace atlas
