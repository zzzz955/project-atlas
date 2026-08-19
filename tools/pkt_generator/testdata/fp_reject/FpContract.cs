// 테스트 픽스처다. 실제 계약이 아니다 — `shared/contracts/` 밖에 두었으므로 `npm run gen:pkt`
//    가 이 파일을 읽지 않는다. ctest 의 `pkt.generator_rejects_fp_*` 가 `--contracts-dir` 로
//    이 디렉터리를 가리켜, 부동소수점 필드를 만난 생성기가 **비제로 exit + 명확한 메시지**로
//    죽는지 확인한다(architecture-design.md §8.4).
//
// 조용히 통과시키면 §8.4(양자화)가 무너지고, 그 사실은 대역폭이 터지는 부하 테스트에 가서야
// 드러난다. 그래서 생성 시점에 게이트를 둔다.

namespace Atlas.Contracts.TestData;

public sealed class FpContract
{
    public ushort ActorSlot { get; set; }

    // 이것이 거부 대상이다.
    public float PositionX { get; set; }
}
