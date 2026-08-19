// core-purity 규칙 2 위반 픽스처 — 검사기가 실제로 잡는지 확인하는 용도다.
// 컴파일 대상이 아니다. `server/atlas/**` 밖에 있으므로 실제 게이트 스캔에는 들어가지 않는다.
#include "atlas/core/types.h"

namespace atlas::core {

// 위반: 코어가 데모 게임 어휘(§3.3)를 안다.
void ApplySkillDamage(UInt64 boss_id) {
  (void)boss_id;
}

}  // namespace atlas::core
