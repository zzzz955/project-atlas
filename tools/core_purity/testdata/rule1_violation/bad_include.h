// core-purity 규칙 1 위반 픽스처 — 검사기가 실제로 잡는지 확인하는 용도다.
// 🔴 컴파일 대상이 아니다. `server/atlas/**` 밖에 있으므로 실제 게이트 스캔에는 들어가지 않는다.
#pragma once

#include "atlas/core/types.h"
#include "generated/pkt/pkt_login.h"  // 위반: 코어가 게임 계약 헤더를 직접 안다
