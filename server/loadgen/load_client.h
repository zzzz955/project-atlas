#pragma once

// =============================================================================
// 부하 하네스의 옵션/결과 계약. 실행 자체는 load_client.cpp
// 좋은 수치가 무엇인지 판정하지 않고 일어난 일만 보고
// =============================================================================

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "atlas/core/types.h"

// [AD 9] 연결 1개 = 스레드 1개가 아니다
// 스레드-퍼-커넥션은 자기 스케줄러를 측정하게 된다
// 서버와 같은 모델을 써야 드러난 한계가 서버의 한계다
// [AD 16.1a] 프레임 계층과 오프코드 표를 재사용한다
// 두 번째 프레이밍 구현은 자기 버그를 달고 와 측정을 디버깅으로 바꾼다

namespace atlas_loadgen
{

struct LoadOptions
{
    // 호스트명이 아니라 리터럴 주소
    // 실행 도중 이름 해석이 멈추면 그 지연이 그대로 표본에 섞인다
    std::string host{ "127.0.0.1" };
    atlas::UInt16 port{ 7777 };

    std::size_t connections{ 1 };

    // 합산 목표 rps. 0 = closed loop
    // 포화점은 closed loop 로만 찾는다
    // 서버가 못 받는 속도의 open loop 는 클라이언트 적체를 지연으로 보고한다
    atlas::UInt32 rate_per_second{ 0 };

    // 워밍업 포함 전체 실행 시간
    atlas::UInt32 duration_seconds{ 20 };
    // 버리는 앞부분
    // 연결 수립, 첫 캐릭터 로드, 드라이버의 첫 prepare 왕복이 여기 들어간다
    // 어느 것도 정상 상태가 아니다
    atlas::UInt32 warmup_seconds{ 3 };

    std::size_t io_threads{ 4 };

    // [AD 10.5] 연결 i 는 first_character_id + i 를 구동한다
    // 그래야 캐릭터별 락에서 경합하지 않는다
    // 한 캐릭터에 몰면 서버가 아니라 그 직렬화 지점을 측정하게 된다
    atlas::UInt64 first_character_id{ 900000 };
    atlas::UInt16 server_id{ 1 };

    // [AD 16.1i] 하네스 쪽 소켓의 TCP_NODELAY. 값 없음 = 손대지 않음
    // 기존 측정이 그러했으므로 기본값은 false 가 아닌 빈 optional
    // [AD 9.3] 서버 소켓이 아니다. 서버는 리터럴로 no_delay(true) 를 건다
    // 런타임 키가 없어 그쪽 대조는 코어 변경이 된다
    // 여기서는 요청 절반만 대조한다
    std::optional< bool > no_delay;

    // [AD 16.1i] 램프업 축. 항목은 단계별 연결 수
    // 연결은 경계에서 합류만 하고 빠지지 않으므로 목록은 순증가여야 한다
    // 비어 있는 것이 기본이다
    // 그 기본이 기존 표를 측정한 고정 동시성 모드다
    // 기본이 바뀌면 문서의 수치로 돌아갈 경로가 사라진다
    std::vector< std::size_t > ramp_stages;
    atlas::UInt32 stage_seconds{ 0 };

    // [AD 16.1a] 관측 축. 둘 다 기본 off
    // 그래야 메트릭 싱크도 샘플러 스레드도 없는 실행이 그대로 재현된다
    // 기존 표와 비교 가능한 것은 그 실행뿐이다
    bool tui{ false };
    std::string sample_jsonl_path;
    // 외부 프로브 루프가 갱신하는 파일 경로(ms). 읽기만 하고 재지 않음
    std::string probe_file_path;
};

// 단계 1개의 정상 상태 구간
// 고정 동시성 실행은 단계가 1개일 뿐 보고 방식이 둘인 것이 아니다
struct StageStats
{
    std::size_t connections{ 0 };
    atlas::UInt32 window_ms{ 0 };
    std::size_t responses_ok{ 0 };
    // [AD 10.8] 실패가 아니라 거부
    // 연결은 유지되고 다음 요청은 정상 처리된다
    // 실패에 더하면 "거절했다" 가 "망가졌다" 로 뒤바뀐다
    std::size_t responses_rejected{ 0 };
    std::size_t responses_refused{ 0 };
    std::vector< atlas::UInt32 > latencies_us;
    // 수락된 작업만 담는다
    // 거부는 us 단위로 돌아와, 다수가 되면 섞인 p50 을 무너뜨린다
    // 그 결과는 "과부하에서 빨라졌다" 로 읽힌다
    // 램프가 묻는 것은 처리된 요청 쪽이다
    std::vector< atlas::UInt32 > ok_latencies_us;
};

struct LoadStats
{
    std::size_t connections_attempted{ 0 };
    std::size_t connections_established{ 0 };
    std::size_t connect_failures{ 0 };
    // 수립 후 실행 종료 전에 끊긴 것 - reset, 프레이밍 에러, 상대 종료.
    // connect 실패와 뜻이 달라 따로 셈
    std::size_t transport_failures{ 0 };
    std::size_t load_failures{ 0 };
    std::size_t peak_live_connections{ 0 };

    std::size_t requests_sent{ 0 };
    std::size_t responses_ok{ 0 };
    // [AD 10.3] 획득 타임아웃 안에 풀이 연결을 못 내준 경우.
    // 풀 포화의 강한 신호라 다른 거절과 따로 셈
    std::size_t responses_unavailable{ 0 };
    std::size_t responses_refused{ 0 };
    // 같은 방식으로 거절된 캐릭터 로드. 세되 재시도하고 치명으로 보지 않는다
    // 유실로 처리하면 램프가 설명하려는 부하 구간에서 동시성이 사라진다
    std::size_t loads_rejected{ 0 };
    // OS 가 --no-delay 요청을 거절한 연결 수
    // 0 이 아니면 그 셀은 자기 라벨과 다른 것을 측정한 것이라 반드시 보고한다
    std::size_t no_delay_failures{ 0 };

    // 정상 상태 equip 왕복, us, 정렬 안 됨
    // 램프업이면 단계 구간의 합집합이다
    // 여기서 나온 처리량은 용량이 아니라 단계 평균이다
    // 답은 아래 stages 가 싣는다
    std::vector< atlas::UInt32 > latencies_us;
    atlas::UInt32 steady_window_ms{ 0 };
    std::vector< StageStats > stages;
    // 돌아오지 않은 연결 때문에 워치독이 io_context 를 멈춘 경우.
    // 조용히 잘라내지 않고 의심 실행으로 보고
    bool watchdog_fired{ false };
};

[[nodiscard]] LoadStats RunLoad( const LoadOptions& options );

// 오름차순 정렬 표본의 nearest-rank 백분위. 빈 벡터면 0.
// fraction 은 50/99 가 아니라 0.50/0.99 - 섞이면 p50 인 p99 가 나옴
[[nodiscard]] atlas::UInt32 Percentile( const std::vector< atlas::UInt32 >& sorted_samples,
                                        atlas::Float64 fraction );

}  // namespace atlas_loadgen
