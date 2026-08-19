#pragma once

// =============================================================================
// [AD 10] ORM 런타임의 우산 헤더
// 값 - 행 - 에러 - 접속 설정 등 공통 어휘와 Connection 을 담음
// =============================================================================

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "atlas/core/error.h"
#include "atlas/core/time.h"
#include "atlas/core/types.h"

// <mysql.h> 는 이 라이브러리의 .cpp 에만 등장
// void* 세탁 대신 드라이버 C API 태그를 그대로 전방 선언
// 태그가 바뀌면 조용한 재해석이 아니라 빌드가 깨짐
// 이름은 libmariadb 소유라 [CS 3] 적용 대상이 아님
// NOLINTNEXTLINE(readability-identifier-naming)
struct st_mysql;

namespace atlas
{

class PreparedStatement;

// =============================================================================
// 공통 어휘
// =============================================================================

// 드라이버 실패. 호출자가 DB 만 따로 catch 하려고 나눈 것
// [AD 11.2a] 예상된 실패는 예외가 아님
// 풀 고갈은 nullopt, 없는 행은 빈 결과
// 여기서 던지는 것은 틀린 문장이거나 응답하지 않는 DB 뿐
class DbException : public Exception
{
public:
    using Exception::Exception;
};

// 제네릭 컬럼 값 1개
// 대안은 SQL 타입별이 아니라 최대 폭 하나씩 - 부호 있으면 Int64, 없으면 UInt64
// 선언 폭으로 좁히는 것은 생성된 ColumnMeta 를 쥔 행 매퍼 몫
// monostate 는 SQL NULL
using DbValue = std::variant< std::monostate, Int64, UInt64, Float64, std::string, SysTime >;

using DbRow = std::vector< DbValue >;  // 문장이 선택한 컬럼 순서의 결과 행

// [AD 5.4] 모든 필드는 .env 의 SecretConfig 출처. server.ini 아님
// 어느 필드도 로그나 예외 메시지에 실리지 않음
// atlas/db 가 자격 증명 때문에 config 계층에 링크하지 않으려고 둔 구조체
struct DbConnectionConfig
{
    std::string host{};
    UInt16 port{ 3306 };
    std::string database{};
    std::string user{};
    std::string password{};

    // 드라이버가 인증 플러그인을 찾는 경로. 비우면 드라이버 빌드 기본값
    // MySQL 8.4 의 caching_sha2_password 는 별도 플러그인 DLL
    // 경로를 못 찾으면 서버는 "1045 access denied" 로만 답함
    // 증상이 자격 증명 오류를 가리키게 됨
    std::string plugin_directory{};

    // 암호화는 하되 서버 인증서를 검증하지 않음
    // 능동 MITM 을 막지 못하므로 운영 금지
    // compose 의 stock mysql:8.4 는 자가 서명 CA 를 만듦
    // Connector/C 3.4.8 이 그 체인을 거부해 GAME 이 기동 중 죽음
    // 이 플래그는 그 상황의 탈출구
    bool tls_no_verify{ false };  // ATLAS_DB_TLS_NO_VERIFY. 켜지면 경고 로그
};

// =============================================================================
// 연결
// =============================================================================

// 드라이버 연결 1개 + 그 연결의 prepared statement 캐시
// [AD 9.1] 스레드 안전 아님. 풀이 한 스레드에 대여하는 동안만 소유됨
// 이것이 풀 아래를 통째로 락 없이 두는 근거
// 공유하면 드라이버 상태가 깨짐
class Connection
{
public:
    // 즉시 접속하고 서버가 거부하면 DbException
    // 재접속용으로 설정을 보관해 어차피 복사가 일어나므로 값 전달
    explicit Connection( DbConnectionConfig config );
    ~Connection();

    Connection( const Connection& ) = delete;
    Connection& operator=( const Connection& ) = delete;
    Connection( Connection&& ) = delete;
    Connection& operator=( Connection&& ) = delete;

    // 사용 가능한지 확인하고 아니면 정확히 1회만 재접속
    // wait_timeout 만료로 풀이 죽은 소켓을 영구히 나눠주는 상태를 막음
    // 재시도 루프를 두지 않는 이유 - DB 정전 동안 모든 DB 스레드가 눌러앉음
    // 그러면 장애가 정지로 바뀌고 복구 중인 서버를 두들기게 됨
    // 지금 실패하고 다음 대여에서 재시도하는 것이 재시도 정책
    [[nodiscard]] bool EnsureAlive();

    // 캐시는 parse/plan 왕복 제거가 목적
    // SQL 인젝션 차단은 텍스트를 런타임에 조립하지 않는 데서 오는 결과
    // 캐시의 존재 이유가 아님
    // 순서를 뒤집으면 "안전한 쿼리" 라며 캐시를 걷어내는 사람이 나옴
    PreparedStatement& Prepare( std::string_view sql );

    // 실제 prepare 횟수. 캐시 적중 회귀 테스트가 검사하는 대상
    [[nodiscard]] UInt64 PrepareCount() const noexcept { return prepare_count_; }

private:
    // [AD 10] 트랜잭션 제어가 private 인 이유
    // 커밋 없이 스코프를 벗어나면 반드시 롤백되어야 함
    // 그것을 보장하는 것은 RAII 스코프뿐
    friend class Transaction;

    void Begin();
    void Commit();
    void Rollback() noexcept;
    void RestoreAutoCommit() noexcept;

    // 드라이버 핸들을 여는 유일한 지점. 생성자와 EnsureAlive 가 공유
    // 재접속이 최초 접속과 다른 옵션을 협상할 수 없음
    void Open();

    // 재접속에 같은 자격 증명이 필요해서만 보관
    // [AD 5.4] 로그에도 예외 메시지에도 절대 실리지 않음
    DbConnectionConfig config_;

    st_mysql* handle_{ nullptr };
    std::unordered_map< std::string, std::unique_ptr< PreparedStatement > > statements_;
    UInt64 prepare_count_{ 0 };
};

}  // namespace atlas
