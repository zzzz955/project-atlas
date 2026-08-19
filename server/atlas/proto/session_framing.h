#pragma once

// =============================================================================
// 바이트 경계와 게임 사이의 이음매
// 세션에 FrameReader 를 꽂고 프레임을 세션 쓰기 경로에 태운다
// =============================================================================

#include <functional>
#include <memory>
#include <span>

#include "atlas/core/types.h"
#include "atlas/proto/frame.h"

namespace atlas
{

// 전방 선언은 고의
// session.h 를 포함하면 프레임 계층을 쓰는 모든 곳에 <boost/asio.hpp> 가 딸려 온다
// 이 라이브러리의 파싱 헤더는 asio 무의존
class Session;

// [AD 14] 코어는 (session, frame) 까지만 넘기고 그 뒤를 모름
// opcode -> 핸들러 표는 게임 쪽 자산
using FrameHandler = std::function< void( const std::shared_ptr< Session >&, const Frame& ) >;

// 세션의 바이트 핸들러로 FrameReader 를 설치. Session::Start() 전에 호출할 것
// [AD 9.1] 리더 소유자는 세션, 접근자는 세션 strand 하나뿐이라 락이 없음
void AttachFrameReader( const std::shared_ptr< Session >& session, FrameHandler on_frame );

// 프레임 1개를 인코드해 세션 쓰기 경로에 큐잉. kMaxPayload 초과면 false
// [AD 8.3] seq 는 호출자가 넘긴다
// 여기 숨긴 카운터는 Send 를 부르는 쪽에서 strand 밖으로 만져진다
// 그것이 strand 모델이 금지하는 경쟁 그 자체
bool SendFrame( Session& session, UInt16 opcode, UInt32 seq, std::span< const Byte > payload );

}  // namespace atlas
