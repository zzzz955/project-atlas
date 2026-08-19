#pragma once

// =============================================================================
// [AD 5.4] 커밋되는 쪽 설정 server.ini 파서. 비밀 값은 .env 담당
// =============================================================================

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace atlas
{

// 손으로 짠 파서. 필요한 문법은 [section], key = value, ;/# 주석뿐
// [AD 15.2] Boost.PropertyTree 는 vcpkg 의존성만 하나 늘림
class IniDocument
{
public:
    // [AD 5.2] 형식 오류는 조용히 건너뛰지 않고 실패 처리
    // role/port 오타 하나가 서버를 엉뚱한 정체성으로 부팅시킴
    // [AD 11.2a] 읽을 수 없는 기동 설정은 복구 불가 상태라 throw
    [[nodiscard]] static IniDocument Parse( std::string_view text );
    [[nodiscard]] static IniDocument LoadFile( std::string_view path );

    [[nodiscard]] std::optional< std::string_view > Get( std::string_view section,
                                                         std::string_view key ) const;

private:
    // std::less<> 로 투명 비교. string_view 조회에 std::string 임시 생성 없음
    using Section = std::map< std::string, std::string, std::less<> >;

    std::map< std::string, Section, std::less<> > sections_;
};

}  // namespace atlas
