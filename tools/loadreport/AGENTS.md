# loadreport — 부하 런 JSONL → 자체완결 HTML

`atlas_loadgen --sample-jsonl <path>` 가 남긴 초당 샘플을 **인라인 SVG 차트가 박힌 HTML 1파일**로
만든다. `docs/design/architecture-design.md §16.1a` 시각화 축의 "제출물용" 절반이고, 나머지 절반인
실시간 TUI(`--tui`)는 C++ 쪽 `server/loadgen/live_view.{h,cpp}` 에 있다.

## 생성기가 아니다

`tools/` 의 다른 셋(`info` · `db` · `pkt`)은 입력으로부터 **커밋되는 소스**를 만들고, 그래서
`gen:check` 드리프트 게이트의 대상이다. 이 도구는 **측정 산출물**을 만든다.

- `gen:all` · `gen:check` 조성에 **넣지 않는다.** 넣으면 게이트가 "누가 언제 돌린 측정의 HTML 이
  최신인가"를 묻게 되는데, 그 질문에는 정답이 없다(입력이 코드가 아니라 그날의 런이다).
- 출력은 `.gitignore` 된다(`reports/` · `*.jsonl` · `*.loadreport.html`). **수치의 SoT 는
  계속 `§16.1` 의 표다** — 그림이 표를 대체하지 않는다.

## 실행

레포 루트에서:

```
npm run loadreport -- --in reports/run.jsonl
npm run loadreport -- --in reports/run.jsonl --out reports/run.html \
                      --title "64커넥션 폐루프" --note "호스트 = i5-12600KF / 32GB"
```

**Windows PowerShell 에서는 `npm run` 을 거치지 말고 스크립트를 직접 부른다 (2026-08-14 실측).**
`npm.ps1` 이 `--` 구분자를 삼켜서 `--in` 이 사라지고 스크립트는 벌거벗은 경로 하나만 받는다 —
`[loadreport] ERROR: 알 수 없는 옵션 'reports\run.jsonl'` 로 죽는다. 인자를 따옴표로 감싸도 같다.

```powershell
node tools\loadreport\loadreport.js --in reports\run.jsonl
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
 "server_id":1,"first_character_id":900000,"ramp":"","stage_seconds":0,"sample_interval_ms":1000}
{"kind":"sample","t":1.001,"rps":132.87,"p50":454.400,"p99":581.100,"ok_p50":454.400,
 "ok_p99":581.100,"inflight":64,"errors":0,"responses":133,"rejections":0,"load_rejections":0,
 "stage":0,"stage_connections":64,"fsync_probe_ms":4.050}
```

- `p50` · `p99` 는 **live_view 의 100 µs 히스토그램**에서 나온다. 런이 최종 보고하는 백분위와
  같은 수가 **아니다** — 그쪽은 정확한 샘플 벡터에서 계산한다(`loadgen/main.cpp` 의 `Report`).
  같은 이름의 두 계산이 조용히 하나가 되면 안 되고, 싼 쪽이 발표되는 수가 되면 더 안 된다.
- `responses` · `rejections` 는 **그 창의 카운트**, `load_rejections` 는 **누계**다. 거부율은
  이 둘의 비이지 `rejections` 단독이 아니다 — 분모 없는 거부 수는 읽을 수 없다.
- `ok_p50` · `ok_p99` 는 **수락된 응답만**의 백분위다. 거부는 아무것도 실행하지 않고 마이크로초
  단위로 답하므로, 거부가 다수가 되는 순간 혼합 `p50` 이 **무너진다** — 1000 ms 에서 2 ms 로
  떨어진 곡선은 "과부하가 서버를 빠르게 만들었다"로 읽힌다. 그림이 쓰는 것은 `ok_*` 쪽이다.
- `stage` · `stage_connections` 는 램프업 단계(`--ramp`)다. 고정 동시성 런에서는 0 과 커넥션 수로
  고정되고, 그때 리포트는 단계 경계선을 그리지 않는다.
- `fsync_probe_ms` 는 **`null` 일 수 있다.** 하네스는 이 값을 재지 않고 `--probe-file` 이 가리키는
  파일을 옮겨 적기만 한다. `§16.1c-①` 의 판별기는 **MySQL 컨테이너 데이터 볼륨**에 대고 도는
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

## 규칙

- **외부 요청 0.** CDN · 웹폰트 · 원격 이미지 · `<script>` 어느 것도 없다(스크립트 태그 자체가
  없다). 오프라인에서 열리지 않으면 제출물로 쓸 수 없다.
- **차트 4개를 넘기지 않는다** — ① 처리량(수락된 것만) ② **거부율** ③ 지연 백분위(수락된 것만,
  p50 · p99) ④ fsync 프로브. 인터랙션 · 필터 · 런 비교 UI 없음. 정지 화면 1장이 하는 일이
  여기서는 전부다.
  **거부율이 4번째가 아니라 2번째인 이유**: 상한 위에서 관측되는 형태는 "지연이 발산한다"가
  아니라 **"처리량은 상한에 눌린 채 거부율이 오른다"** 이고(`§10.8` · `§16.1i`), 그 문장을
  지지하는 계열은 이것 하나다. 거부율 없는 처리량 곡선은 "상한에서 눌린 것"과 "거부로 흘린 것"을
  구분하지 못한다.
- **서버측 카운터는 `--note` 로 들어간다.** 클라이언트가 센 거부와 **서버가 센 거부**(§10.8 의
  5초 주기 카운터 로그)가 일치하는지가 이 페이지의 검산이고, 어긋나면 그 자체가 보고 대상이다
  (둘 중 하나가 틀렸다는 뜻이다). 조건표의 "거부 (클라이언트 집계)" 행은 자동으로 채워지므로
  나란히 놓을 것은 서버 쪽 수 하나다.
- **측정 조건을 같은 페이지에 싣는다.** 조건 없는 그림은 `§16.1b` 가 말한 조건 없는 표와 똑같이
  의심받는다. 하네스가 아는 조건은 `meta` 레코드에서 자동으로, 모르는 조건(호스트 · 컨테이너 ·
  MySQL 내구성 설정)은 `--note` 로 들어가며 SoT 는 `§16.1b` 다.
- **색은 `dataviz` 스킬의 검증된 기본 팔레트 슬롯 1 · 2**(`#2a78d6` / `#eb6834`, 다크 스텝
  `#3987e5` / `#d95926`). 두 슬롯 조합을 `validate_palette.js` 로 light · dark 양쪽 all-pairs
  검증했고 전 항목 PASS 다. 색을 바꾸려면 검증기를 다시 돌린다 — 눈으로 고르지 않는다.
- **값이 없는 구간에서 선을 끊는다.** 프로브가 `null` 인 구간을 이어 그리면 없는 관측을 지어낸다.
- **샘플 0개면 exit 1.** 빈 그림은 "부하가 0이었다"로 읽히고 진짜 원인(런이 1초보다 짧았다)을 감춘다.
