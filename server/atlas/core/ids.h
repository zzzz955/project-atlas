#pragma once

// =============================================================================
// [CS 4.3] 프레임워크 식별자 4종
// alias 였다면 인자 뒤바뀜이 그대로 컴파일되고 런타임에 조용히 틀린다
// enum class 는 컴파일 에러
// =============================================================================

#include <concepts>

#include "atlas/core/types.h"

namespace atlas
{

// [AD 6] 신원 사다리는 account -> server -> character
// server_id 는 아직 강타입이 아니다
// 인자로 넘기는 곳이 없어 막을 실수 자체가 없다
enum class AccountId : UInt64
{
};
enum class CharacterId : UInt64
{
};
enum class SessionId : UInt64
{
};
enum class ActorId : UInt64
{
};

// 원시 값 추출의 유일한 통로
// 호출부에서 static_cast 가 사라진다
// 떠도는 캐스트가 리뷰에서 곧바로 냄새가 된다
// [CS 6] 제약은 concept. 4종 외 임의 enum 의 세탁을 막는다
template < class T >
concept StrongId = std::same_as< T, AccountId > || std::same_as< T, CharacterId > ||
                   std::same_as< T, SessionId > || std::same_as< T, ActorId >;

template < StrongId Id >
constexpr UInt64 IdValue( Id id ) noexcept
{
    return static_cast< UInt64 >( id );
}

}  // namespace atlas
