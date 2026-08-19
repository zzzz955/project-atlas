// =============================================================================
// server.ini 파싱 구현. 형식 오류는 하나도 빠짐없이 기동 실패로 처리
// =============================================================================

#include "atlas/config/ini_document.h"

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "atlas/core/error.h"
#include "atlas/core/types.h"

namespace atlas
{
namespace
{

constexpr std::string_view kIniBlankChars = " \t\r\n";

std::string_view TrimIniField( std::string_view text )
{
    const std::size_t first = text.find_first_not_of( kIniBlankChars );
    if ( first == std::string_view::npos )
    {
        return {};
    }
    const std::size_t last = text.find_last_not_of( kIniBlankChars );
    return text.substr( first, last - first + 1 );
}

// 인용/이스케이프 없음. server.ini 의 어떤 값도 ; 나 # 를 담지 않음
std::string_view StripIniComment( std::string_view line )
{
    const std::size_t comment = line.find_first_of( ";#" );
    return comment == std::string_view::npos ? line : line.substr( 0, comment );
}

}  // namespace

IniDocument IniDocument::Parse( std::string_view text )
{
    IniDocument document;
    std::string current_section;
    bool inside_section = false;
    UInt32 line_number = 0;
    std::size_t cursor = 0;

    while ( cursor <= text.size() )
    {
        const std::size_t newline = text.find( '\n', cursor );
        const std::string_view raw = text.substr(
            cursor, newline == std::string_view::npos ? std::string_view::npos : newline - cursor );
        cursor = ( newline == std::string_view::npos ) ? text.size() + 1 : newline + 1;
        ++line_number;

        const std::string_view line = TrimIniField( StripIniComment( raw ) );
        if ( line.empty() )
        {
            continue;
        }

        if ( line.front() == '[' )
        {
            ATLAS_CHECK( line.size() >= 3 && line.back() == ']',
                         "server.ini line {}: section header is not closed with ']'", line_number );
            current_section = std::string( TrimIniField( line.substr( 1, line.size() - 2 ) ) );
            ATLAS_CHECK( !current_section.empty(), "server.ini line {}: empty section name",
                         line_number );
            inside_section = true;
            continue;
        }

        const std::size_t separator = line.find( '=' );
        ATLAS_CHECK( separator != std::string_view::npos,
                     "server.ini line {}: expected 'key = value'", line_number );
        // [section] 앞의 키는 소속이 없음. 추측해 넣으면 읽는 사람 기대와 어긋남
        ATLAS_CHECK( inside_section, "server.ini line {}: key outside of any [section]",
                     line_number );

        std::string key( TrimIniField( line.substr( 0, separator ) ) );
        ATLAS_CHECK( !key.empty(), "server.ini line {}: empty key", line_number );
        std::string value( TrimIniField( line.substr( separator + 1 ) ) );

        Section& section = document.sections_[current_section];
        const bool inserted = section.emplace( std::move( key ), std::move( value ) ).second;
        // last-one-wins 는 중복을 숨김. role 을 두 줄 쓴 운영자에게 알려야 함
        ATLAS_CHECK( inserted, "server.ini line {}: duplicate key in section [{}]", line_number,
                     current_section );
    }

    return document;
}

IniDocument IniDocument::LoadFile( std::string_view path )
{
    std::ifstream file( std::string( path ), std::ios::binary );
    ATLAS_CHECK( file.is_open(), "server.ini could not be opened: {}", path );

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return Parse( buffer.str() );
}

std::optional< std::string_view > IniDocument::Get( std::string_view section,
                                                    std::string_view key ) const
{
    const auto section_it = sections_.find( section );
    if ( section_it == sections_.end() )
    {
        return std::nullopt;
    }
    const auto key_it = section_it->second.find( key );
    if ( key_it == section_it->second.end() )
    {
        return std::nullopt;
    }
    return std::string_view{ key_it->second };
}

}  // namespace atlas
