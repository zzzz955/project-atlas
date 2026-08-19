#pragma once

// =============================================================================
// [AD 5.4] 설정 경계의 .env 쪽. 커밋되는 server.ini 에는 절대 안 들어감
// 여기 값은 로그/예외 메시지/스트림 어디에도 나가지 않음
// =============================================================================

#include <string>

#include "atlas/core/types.h"

namespace atlas
{

struct SecretConfig
{
    std::string db_host;
    UInt16 db_port{ 0 };
    std::string db_name;
    std::string db_user;
    std::string db_password;
    std::string jwks_url;

    // [AD 10.2] 접속 정보만 담는다
    // Redis 를 무엇에 쓸지는 설계 규칙이지 설정이 아니다
    // 금지된 용도를 켤 손잡이는 여기 없음. 빈 host = 캐시 미설정
    std::string redis_host;
    UInt16 redis_port{ 0 };
    std::string redis_password;

    // [AD 5.4] 비밀은 아니지만 배포마다 다른 축이라 server.ini 로는 못 담음
    // 기본 false = 검증함. 문자열 "1" 만 켜므로 오타는 아무것도 완화 못 함
    bool db_tls_no_verify{ false };

    // 미설정 키는 실패가 아니라 빈 값. 필수 여부는 쓰는 계층이 판단
    [[nodiscard]] static SecretConfig FromEnvironment();

    [[nodiscard]] std::string DescribePresence() const;

    void LogSummary() const;
};

}  // namespace atlas
