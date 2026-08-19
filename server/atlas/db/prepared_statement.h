#pragma once

// =============================================================================
// [AD 10] 런타임에 SQL 을 조립하지 않음
// 문장은 ? 자리표시자를 가진 고정 텍스트. 실행은 바인딩 파라미터로만
// =============================================================================

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atlas/core/types.h"
#include "atlas/db/connection.h"

// 드라이버의 문장 핸들. connection.h 가 밝힌 이유로 C API 태그를 그대로 씀
// <mysql.h> 는 이 라이브러리의 어느 헤더에도 들어오지 않음
// NOLINTNEXTLINE(readability-identifier-naming)
struct st_mysql_stmt;

namespace atlas
{

// prepare 한 Connection 이 소유하고 SQL 텍스트로 캐시하는 문장 1개
// 문장은 연결보다 오래 살지 않음. Connection 소멸자가 캐시를 먼저 허무는 이유
class PreparedStatement
{
public:
    // 즉시 prepare 하고 서버가 텍스트를 거부하면 DbException
    // 드라이버 핸들을 쥔 유일한 호출자인 Connection::Prepare 만 생성
    PreparedStatement( st_mysql* connection, std::string sql );
    ~PreparedStatement();

    PreparedStatement( const PreparedStatement& ) = delete;
    PreparedStatement& operator=( const PreparedStatement& ) = delete;
    PreparedStatement( PreparedStatement&& ) = delete;
    PreparedStatement& operator=( PreparedStatement&& ) = delete;

    // 서버가 보고한 ? 개수
    // 생성된 헤더가 자신의 바인딩 배열과 같은 수인지 static_assert 함
    // 여기서 어긋나면 그 문장은 생성된 것이 아님
    [[nodiscard]] std::size_t ParameterCount() const noexcept { return parameter_count_; }
    [[nodiscard]] std::string_view Sql() const noexcept { return sql_; }

    // 행을 반환하지 않는 문장. 결과는 영향받은 행 수
    UInt64 Execute( std::span< const DbValue > parameters );

    // 행을 반환하는 문장을 실행하고 결과 집합 전체를 물질화
    // 스트리밍 커서면 호출자 프레임이 끝난 뒤에도 풀 대여분 연결을 붙듦
    // Phase-1 결과 집합은 캐릭터 하나 분량이라 복사는 문제되는 비용이 아님
    std::vector< DbRow > Query( std::span< const DbValue > parameters );

private:
    void BindAndExecute( std::span< const DbValue > parameters );

    std::string sql_;
    st_mysql_stmt* stmt_{ nullptr };
    std::size_t parameter_count_{ 0 };
};

}  // namespace atlas
