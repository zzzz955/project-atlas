// project-atlas 데모 계약 — 이동 (architecture-design.md §3.3 "이동", §8 프로토콜).
//
// 이 파일은 계약 SoT다. 여기서 C++ DTO(server/generated/pkt/**)가 생성된다 —
//    `npm run gen:pkt`. 생성물을 직접 고치지 말고 이 파일을 고친다.
// C++ 서버 레포에 .cs 가 있는 것은 의도된 것이다: 계약 한 벌 → C#/GDScript/C++ 다중 타깃
//    생성이 이 프레임워크의 seam 이다(architecture-design.md §14).
// 부동소수점 필드 금지(§8.4). 좌표는 그리드 단위로 양자화된 정수다.
// 프레임 헤더(길이·opcode·시퀀스·체크섬·HMAC)는 계약에 넣지 않는다 — 코어의 몫이다.

using System.Collections.Generic;

namespace Atlas.Contracts;

public enum MoveDirection : byte
{
    None = 0,
    North = 1,
    East = 2,
    South = 3,
    West = 4
}

/// <summary>클라 → 서버 이동 입력 1건.</summary>
public sealed class MoveInputRequest
{
    /// <summary>클라이언트 로컬 틱. 프레임 시퀀스 넘버와는 별개다.</summary>
    public uint ClientTick { get; set; }

    public MoveDirection Direction { get; set; }

    /// <summary>양자화된 그리드 좌표(§8.4). 셀 단위 정수다.</summary>
    public short GridX { get; set; }

    public short GridY { get; set; }

    public bool Sprinting { get; set; }
}

/// <summary>서버 → 클라 AoI 이동 스냅샷. 구조체 배열이 아니라 배열들의 구조체(SoA)로 둔다 —
/// 같은 필드가 붙어 있어야 델타/압축이 먹는다(§8.4).</summary>
public sealed class MoveStateBroadcast
{
    public uint ServerTick { get; set; }

    public List<ulong> ActorIds { get; set; } = new();

    public List<short> GridXs { get; set; } = new();

    public List<short> GridYs { get; set; } = new();
}
