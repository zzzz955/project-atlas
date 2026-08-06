// project-atlas 데모 계약 — 채팅 (architecture-design.md §3.3 "채팅").
//
// 브로드캐스트 경로를 증명하기 위한 최소 계약이다. 가변 길이 문자열이 들어 있으므로
// 길이 프리픽스(UInt16 LE) + UTF-8 인코딩 경로를 함께 행사한다(§8.5).
// 🔴 생성물을 직접 고치지 말고 이 파일을 고친 뒤 `npm run gen:pkt`.

namespace Atlas.Contracts;

public enum ChatChannel : byte
{
    Say = 0,
    World = 1
}

/// <summary>서버 → 클라 채팅 브로드캐스트.</summary>
public sealed class ChatBroadcast
{
    public ulong SpeakerActorId { get; set; }

    public ChatChannel Channel { get; set; }

    /// <summary>UTF-8. 직렬화 시 UInt16 길이 프리픽스가 앞에 붙는다(바이트 수, 코드포인트 수 아님).</summary>
    public string Text { get; set; } = string.Empty;
}
