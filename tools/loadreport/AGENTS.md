# loadreport — 부하 런 JSONL → 자체완결 HTML

`atlas_loadgen --sample-jsonl <path>` 가 남긴 초당 샘플을 **인라인 SVG 차트가 박힌 HTML 1파일**로
만든다. `docs/design/architecture-design.md §16.1a` 시각화 축의 "제출물용" 절반이고, 나머지 절반인
실시간 TUI(`--tui`)는 C++ 쪽 `server/loadgen/live_view.{h,cpp}` 에 있다.

## 🔴 생성기가 아니다

`tools/` 의 다른 셋(`info` · `db` · `pkt`)은 입력으로부터 **커밋되는 소스**를 만들고, 그래서
`gen:check` 드리프트 게이트의 대상이다. 이 도구는 **측정 산출물**을 만든다.

- `gen:all` · `gen:check` 조성에 **넣지 않는다.** 넣으면 게이트가 "누가 언제 돌린 측정의 HTML 이
  최신인가"를 묻게 되는데, 그 질문에는 정답이 없다(입력이 코드가 아니라 그날의 런이다).
- 출력은 `.gitignore` 된다(`reports/` · `*.jsonl` · `*.loadreport.html`). 🔴 **수치의 SoT 는
  계속 `§16.1` 의 표다** — 그림이 표를 대체하지 않는다.

## 실행

레포 루트에서:

```
npm run loadreport -- --in reports/run.jsonl
npm run loadreport -- --in reports/run.jsonl --out reports/run.html \
                      --title "64커넥션 폐루프" --note "호스트 = i5-12600KF / 32GB"
```

| 옵션 | 뜻 |
|---|---|
| `--in <path>` | 필수. `--sample-jsonl` 이 남긴 JSONL |
| `--out <path>` | 생략 시 입력과 같은 디렉터리의 `<이름>.loadreport.html` |
| `--title <text>` | 페이지 제목. 생략 시 입력 파일명 |
| `--note "키 = 값"` | 조건표에 한 줄 추가. 반복 가능 |

## 입력 형식

JSONL 1줄 = 1레코드. 하네스가 쓰는 것은 두 종류뿐이다.

```jsonc
{"kind":"meta","harness":"atlas_loadgen","connections":64,"rate_per_second":0,
 "duration_seconds":45,"warmup_seconds":15,"io_threads":8,"host":"127.0.0.1","port":7777,
 "server_id":1,"first_character_id":900000,"sample_interval_ms":1000}
{"kind":"sample","t":1.001,"rps":132.87,"p50":454.400,"p99":581.100,
 "inflight":64,"errors":0,"fsync_probe_ms":4.050}
```

- `p50` · `p99` 는 **live_view 의 100 µs 히스토그램**에서 나온다. 🔴 런이 최종 보고하는 백분위와
  같은 수가 **아니다** — 그쪽은 정확한 샘플 벡터에서 계산한다(`loadgen/main.cpp` 의 `Report`).
  같은 이름의 두 계산이 조용히 하나가 되면 안 되고, 싼 쪽이 발표되는 수가 되면 더 안 된다.
- `fsync_probe_ms` 는 **`null` 일 수 있다.** 하네스는 이 값을 재지 않고 `--probe-file` 이 가리키는
  파일을 옮겨 적기만 한다. 🔴 `§16.1c-①` 의 판별기는 **MySQL 컨테이너 데이터 볼륨**에 대고 도는
  `O_DSYNC` 프로브이고, 클라이언트가 자기 쪽 디스크를 재면 다른 경로를 재는 데다 측정 중인 호스트에
  동기 쓰기를 얹는다. 프로브 루프는 하네스 밖에 둔다:

```powershell
# 별도 창에서, 런 내내
while ($true) {
  $out = docker exec project-atlas-mysql-1 sh -c `
    'dd if=/dev/zero of=/var/lib/mysql/.atlas_fsprobe bs=4096 count=150 oflag=dsync 2>&1; rm -f /var/lib/mysql/.atlas_fsprobe'
  # dd 의 "N bytes copied, T s" 에서 T 를 뽑아 밀리초/회 로 환산해 파일에 쓴다
}
```

## 🔴 규칙

- **외부 요청 0.** CDN · 웹폰트 · 원격 이미지 · `<script>` 어느 것도 없다(스크립트 태그 자체가
  없다). 오프라인에서 열리지 않으면 제출물로 쓸 수 없다.
- **차트 3개를 넘기지 않는다** — ① 처리량 ② 지연 백분위(p50 · p99) ③ fsync 프로브.
  🔴 인터랙션 · 필터 · 런 비교 UI 없음. 정지 화면 1장이 하는 일이 여기서는 전부다.
- **측정 조건을 같은 페이지에 싣는다.** 조건 없는 그림은 `§16.1b` 가 말한 조건 없는 표와 똑같이
  의심받는다. 하네스가 아는 조건은 `meta` 레코드에서 자동으로, 모르는 조건(호스트 · 컨테이너 ·
  MySQL 내구성 설정)은 `--note` 로 들어가며 SoT 는 `§16.1b` 다.
- **색은 `dataviz` 스킬의 검증된 기본 팔레트 슬롯 1 · 2**(`#2a78d6` / `#eb6834`, 다크 스텝
  `#3987e5` / `#d95926`). 두 슬롯 조합을 `validate_palette.js` 로 light · dark 양쪽 all-pairs
  검증했고 전 항목 PASS 다. 🔴 색을 바꾸려면 검증기를 다시 돌린다 — 눈으로 고르지 않는다.
- **값이 없는 구간에서 선을 끊는다.** 프로브가 `null` 인 구간을 이어 그리면 없는 관측을 지어낸다.
- **샘플 0개면 exit 1.** 빈 그림은 "부하가 0이었다"로 읽히고 진짜 원인(런이 1초보다 짧았다)을 감춘다.
