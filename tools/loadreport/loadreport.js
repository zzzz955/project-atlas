#!/usr/bin/env node
'use strict';

// loadreport — atlas_loadgen --sample-jsonl <path> 의 산출물을 자체완결 HTML 1파일로 만든다.
// `docs/design/architecture-design.md §16.1a` 의 시각화 축 중 "제출물용" 쪽이다.
//
// 🔴 생성기가 아니다. `tools/` 의 다른 셋(info · db · pkt)과 달리 입력으로부터 소스를 만들지 않고,
//    따라서 `gen:all` · `gen:check` 조성에 들어가지 않는다 — 드리프트 게이트에 리포트 도구를 넣으면
//    게이트가 "측정 산출물이 최신인가"를 묻게 되고, 그것은 게이트가 답할 수 있는 질문이 아니다.
//
// 🔴 외부 요청 0. CDN · 폰트 · 스크립트 · 원격 이미지 어느 것도 참조하지 않으며 <script> 자체가
//    없다. 오프라인에서 열리지 않는 파일은 제출물이 아니고, `tools/` 파이프라인의 무의존 규칙과도
//    같은 선이다.
//
// 🔴 이 페이지는 `§16.1` 의 수치표를 대체하지 않는다. 표가 SoT 이고 여기 그림은 한 런의 시계열이다.
//
// 사용:
//   npm run loadreport -- --in run.jsonl [--out run.html] [--title "..."] [--note "키 = 값"] ...

const fs = require('node:fs');
const path = require('node:path');

// dataviz 스킬의 검증된 기본 팔레트에서 categorical 슬롯 1 · 2 (light / dark).
// 두 슬롯 조합은 validate_palette.js 에서 light · dark 양쪽 전 항목 PASS
// (all-pairs CVD ΔE 24.7 light / 26.8 dark, 정상시야 ΔE 33.6 / 31.8, 대비 3:1 이상).
const SERIES = [
    { light: '#2a78d6', dark: '#3987e5' },
    { light: '#eb6834', dark: '#d95926' },
];

// ---------------------------------------------------------------- 입력

function parseArgs(argv) {
    const options = { input: '', output: '', title: '', notes: [] };
    for (let index = 0; index < argv.length; index += 1) {
        const key = argv[index];
        const value = argv[index + 1];
        if (key === '--in' || key === '--out' || key === '--title' || key === '--note') {
            if (value === undefined) {
                fail(`옵션 '${key}' 에 값이 없다`);
            }
            index += 1;
            if (key === '--in') options.input = value;
            else if (key === '--out') options.output = value;
            else if (key === '--title') options.title = value;
            else options.notes.push(value);
        } else {
            fail(`알 수 없는 옵션 '${key}'`);
        }
    }
    if (!options.input) {
        fail('--in <path.jsonl> 이 필요하다');
    }
    if (!options.output) {
        const parsed = path.parse(options.input);
        options.output = path.join(parsed.dir, `${parsed.name}.loadreport.html`);
    }
    return options;
}

function fail(message) {
    process.stderr.write(`[loadreport] ERROR: ${message}\n`);
    process.exit(1);
}

// JSONL: 1줄 = 1레코드. `kind` 가 meta 인 것이 런 조건, sample 인 것이 초당 관측.
function readSamples(inputPath) {
    if (!fs.existsSync(inputPath)) {
        fail(`입력이 없다: ${inputPath}`);
    }
    const lines = fs.readFileSync(inputPath, 'utf8').split(/\r?\n/);
    const samples = [];
    let meta = null;
    lines.forEach((line, offset) => {
        const trimmed = line.trim();
        if (!trimmed) return;
        let record;
        try {
            record = JSON.parse(trimmed);
        } catch (error) {
            fail(`${inputPath}:${offset + 1} 을 JSON 으로 읽지 못했다 — ${error.message}`);
        }
        if (record.kind === 'meta') meta = record;
        else if (record.kind === 'sample') samples.push(record);
    });
    if (samples.length === 0) {
        // 🔴 조용히 빈 페이지를 만들지 않는다. 그림이 비어 있는 리포트는 "부하가 0이었다"로
        // 읽히고, 실제 원인(런이 너무 짧아 샘플이 없었다)을 감춘다.
        fail(`${inputPath} 에 sample 레코드가 없다 — 런이 1초보다 짧았는가?`);
    }
    return { meta, samples };
}

// ---------------------------------------------------------------- 차트

const CHART = { width: 760, height: 230, left: 62, right: 92, top: 18, bottom: 30 };

function niceCeiling(value) {
    if (!(value > 0)) return 1;
    const magnitude = 10 ** Math.floor(Math.log10(value));
    const steps = [1, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10];
    for (const step of steps) {
        if (value <= step * magnitude) return step * magnitude;
    }
    return 10 * magnitude;
}

function formatNumber(value, digits) {
    if (value === null || value === undefined || Number.isNaN(value)) return '—';
    return value.toFixed(digits);
}

function escapeHtml(text) {
    return String(text)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
}

// 값이 null 인 구간에서 선을 끊는다 — 없는 값을 이어 그리면 없는 관측을 지어내는 것이다.
function polylineSegments(points) {
    const segments = [];
    let current = [];
    for (const point of points) {
        if (point.y === null) {
            if (current.length > 0) segments.push(current);
            current = [];
        } else {
            current.push(point);
        }
    }
    if (current.length > 0) segments.push(current);
    return segments;
}

// 램프업 단계 경계. 🔴 단계 라벨이 없으면 거부율이 오르는 순간이 "언제부터"인지 알 수 없고,
// 그 답(동시성 N에서 큐 상한을 넘었다)이 이 그림의 전부다.
function stageBands(marks, toX, top, plotHeight, plotRight) {
    if (marks.length < 2) return [];
    const parts = [];
    marks.forEach((mark, index) => {
        if (index === 0) return;
        const x = toX(mark.t);
        if (x > plotRight) return;
        parts.push(
            `<line x1="${x.toFixed(1)}" y1="${top}" x2="${x.toFixed(1)}" ` +
                `y2="${top + plotHeight}" stroke="var(--axis)" stroke-width="1" ` +
                `stroke-dasharray="3 3" />`,
            `<text x="${(x + 4).toFixed(1)}" y="${(top + plotHeight - 6).toFixed(1)}" ` +
                `class="ax">${escapeHtml(String(mark.connections))}</text>`,
        );
    });
    return parts;
}

// 시계열 1개짜리 SVG. series = [{ label, values: [{x, y}], slot, digits }]
function lineChart(options) {
    const { width, height, left, right, top, bottom } = CHART;
    const plotWidth = width - left - right;
    const plotHeight = height - top - bottom;

    const xMax = Math.max(...options.series.flatMap((s) => s.values.map((v) => v.x)), 1);
    const yObserved = Math.max(
        ...options.series.flatMap((s) => s.values.map((v) => (v.y === null ? 0 : v.y))),
        0,
    );
    const yMax = options.yMax !== undefined ? options.yMax : niceCeiling(yObserved * 1.1);
    const toX = (x) => left + (x / xMax) * plotWidth;
    const toY = (y) => top + plotHeight - (y / yMax) * plotHeight;

    const parts = [];
    // 워밍업 구간: 하네스가 버리는 접두부(§16.1a). 그림에는 남기고 음영으로 구분한다 —
    // 🔴 레짐 전환(§16.1c-①)은 바로 이 근처에서 시작하므로 잘라내면 서사가 사라진다.
    if (options.warmupSeconds > 0) {
        const warmupWidth = Math.min(toX(options.warmupSeconds), left + plotWidth) - left;
        parts.push(
            `<rect x="${left}" y="${top}" width="${warmupWidth.toFixed(1)}" ` +
                `height="${plotHeight}" fill="var(--wash)" />`,
            `<text x="${(left + 6).toFixed(1)}" y="${(top + 13).toFixed(1)}" ` +
                `class="ax">워밍업 ${options.warmupSeconds}s (표에서 버려지는 구간)</text>`,
        );
    }

    for (let tick = 0; tick <= 4; tick += 1) {
        const value = (yMax / 4) * tick;
        const y = toY(value);
        parts.push(
            `<line x1="${left}" y1="${y.toFixed(1)}" x2="${left + plotWidth}" ` +
                `y2="${y.toFixed(1)}" stroke="var(--grid)" stroke-width="1" />`,
            `<text x="${left - 8}" y="${(y + 4).toFixed(1)}" class="ax" ` +
                `text-anchor="end">${formatNumber(value, options.tickDigits)}</text>`,
        );
    }
    parts.push(
        `<line x1="${left}" y1="${top + plotHeight}" x2="${left + plotWidth}" ` +
            `y2="${top + plotHeight}" stroke="var(--axis)" stroke-width="1" />`,
    );
    parts.push(...stageBands(options.stageMarks || [], toX, top, plotHeight, left + plotWidth));
    for (let tick = 0; tick <= 4; tick += 1) {
        const seconds = (xMax / 4) * tick;
        parts.push(
            `<text x="${toX(seconds).toFixed(1)}" y="${height - 10}" class="ax" ` +
                `text-anchor="middle">${Math.round(seconds)}s</text>`,
        );
    }

    options.series.forEach((series) => {
        const color = `var(--series-${series.slot})`;
        for (const segment of polylineSegments(series.values)) {
            const d = segment
                .map((point, index) => {
                    const command = index === 0 ? 'M' : 'L';
                    return `${command}${toX(point.x).toFixed(1)} ${toY(point.y).toFixed(1)}`;
                })
                .join(' ');
            parts.push(
                `<path d="${d}" fill="none" stroke="${color}" stroke-width="2" ` +
                    `stroke-linejoin="round" stroke-linecap="round" />`,
            );
        }
        // 끝점 직접 라벨: 2계열 차트에서 범례와 함께 색 하나로 정체성을 지지 않게 한다.
        const last = [...series.values].reverse().find((point) => point.y !== null);
        if (last) {
            parts.push(
                `<circle cx="${toX(last.x).toFixed(1)}" cy="${toY(last.y).toFixed(1)}" r="3.5" ` +
                    `fill="${color}" stroke="var(--surface)" stroke-width="2" />`,
                `<text x="${(toX(last.x) + 9).toFixed(1)}" y="${(toY(last.y) + 4).toFixed(1)}" ` +
                    `class="end">${escapeHtml(series.label)} ` +
                    `${formatNumber(last.y, series.digits)}</text>`,
            );
        }
    });

    const legend =
        options.series.length > 1
            ? `<p class="legend">${options.series
                  .map(
                      (series) =>
                          `<span class="key"><i style="background:var(--series-${series.slot})">` +
                          `</i>${escapeHtml(series.label)}</span>`,
                  )
                  .join('')}</p>`
            : '';

    return (
        `<figure class="chart">` +
        `<figcaption><h2>${escapeHtml(options.title)}</h2>` +
        `<p class="sub">${escapeHtml(options.subtitle)}</p></figcaption>` +
        legend +
        `<div class="plot"><svg viewBox="0 0 ${width} ${height}" role="img" ` +
        `aria-label="${escapeHtml(options.title)}">${parts.join('')}</svg></div>` +
        `</figure>`
    );
}

function emptyChart(title, subtitle, message) {
    return (
        `<figure class="chart">` +
        `<figcaption><h2>${escapeHtml(title)}</h2>` +
        `<p class="sub">${escapeHtml(subtitle)}</p></figcaption>` +
        `<p class="empty">${escapeHtml(message)}</p>` +
        `</figure>`
    );
}

// ---------------------------------------------------------------- 페이지

function conditionRows(meta, notes, samples) {
    const rows = [];
    if (meta) {
        rows.push(['커넥션 수', String(meta.connections)]);
        rows.push([
            '모드',
            meta.rate_per_second === 0
                ? '폐루프 (--rate 0)'
                : `오픈루프 목표 ${meta.rate_per_second} req/s`,
        ]);
        if (meta.ramp) {
            rows.push(['램프 단계', `${meta.ramp} 커넥션 · 단계당 ${meta.stage_seconds}s`]);
        }
        rows.push(['런 길이 · 워밍업', `${meta.duration_seconds}s · ${meta.warmup_seconds}s`]);
        rows.push(['클라이언트 I/O 스레드', String(meta.io_threads)]);
        rows.push(['대상', `${meta.host}:${meta.port} (server_id=${meta.server_id})`]);
        rows.push(['샘플 간격', `${meta.sample_interval_ms} ms`]);
    }
    rows.push(['수집된 샘플', `${samples.length} 개`]);
    // 🔴 클라이언트가 센 거부 총계. 서버가 센 것(§10.8 카운터 로그)과 맞는지는 --note 로 같이
    //    싣는다 — 두 수가 갈리면 둘 중 하나가 틀렸다는 뜻이고 그것이 보고 대상이다.
    if (samples.some((s) => s.responses !== undefined)) {
        const responses = samples.reduce((sum, s) => sum + (s.responses ?? 0), 0);
        const rejections = samples.reduce((sum, s) => sum + (s.rejections ?? 0), 0);
        const share = responses > 0 ? ((rejections / responses) * 100).toFixed(2) : '0.00';
        rows.push([
            '거부 (클라이언트 집계)',
            `${rejections} / ${responses} 응답 = ${share} % · 로드 거부 누계 ` +
                `${samples[samples.length - 1].load_rejections ?? 0}`,
        ]);
    }
    for (const note of notes) {
        const split = note.indexOf('=');
        if (split > 0) rows.push([note.slice(0, split).trim(), note.slice(split + 1).trim()]);
        else rows.push(['비고', note]);
    }
    return rows;
}

// 단계 경계 = `stage` 필드가 바뀌는 첫 샘플. 🔴 meta 에서 계산하지 않고 관측에서 뽑는다 —
// 계획된 스케줄이 아니라 실제로 그 초에 무엇이 돌았는지가 그림의 x축이다.
function stageMarks(samples) {
    const marks = [];
    let previous = null;
    for (const sample of samples) {
        const stage = sample.stage;
        if (stage === undefined || stage === previous) continue;
        previous = stage;
        marks.push({ t: sample.t, connections: sample.stage_connections ?? 0 });
    }
    return marks;
}

function buildHtml(options, meta, samples) {
    const warmup = meta ? Number(meta.warmup_seconds) : 0;
    const marks = stageMarks(samples);
    const isRamp = marks.length > 1;

    // 🔴 수락된 것만. 거부는 아무것도 실행하지 않고 즉시 답하므로 전체 응답률에 섞으면 곡선이
    //    과부하 구간에서 두 자릿수 배로 뛴다 — "빨라졌다"로 읽히는 그 형태다.
    const accepted = samples.map((s) => {
        if (s.responses === undefined) return { x: s.t, y: s.rps };
        const rejected = s.rejections ?? 0;
        const ratio = s.responses > 0 ? (s.responses - rejected) / s.responses : 0;
        return { x: s.t, y: s.rps * ratio };
    });
    const rejectPercent = samples.map((s) => {
        if (s.responses === undefined) return { x: s.t, y: null };
        return {
            x: s.t,
            y: s.responses > 0 ? ((s.rejections ?? 0) / s.responses) * 100 : 0,
        };
    });
    const hasRejectSeries = rejectPercent.some((point) => point.y !== null);
    const okP50 = samples.map((s) => ({ x: s.t, y: s.ok_p50 === undefined ? s.p50 : s.ok_p50 }));
    const okP99 = samples.map((s) => ({ x: s.t, y: s.ok_p99 === undefined ? s.p99 : s.ok_p99 }));
    const probe = samples.map((s) => ({
        x: s.t,
        y: s.fsync_probe_ms === null || s.fsync_probe_ms === undefined ? null : s.fsync_probe_ms,
    }));
    const hasProbe = probe.some((point) => point.y !== null);

    const stageNote = isRamp ? ' · 점선 = 램프 단계 경계, 숫자는 그 단계의 동시 커넥션' : '';
    const charts = [
        lineChart({
            title: '처리량 — 수락된 요청',
            subtitle: `서버가 실제로 실행한 장착 왕복 (req/s)${stageNote}`,
            warmupSeconds: warmup,
            tickDigits: 0,
            stageMarks: marks,
            series: [{ label: 'ok/s', slot: 1, digits: 1, values: accepted }],
        }),
        hasRejectSeries
            ? lineChart({
                  title: '거부율',
                  subtitle:
                      '응답 중 kEquipResponseUnavailable 의 비율 (%) · DB 큐 상한을 넘긴 ' +
                      `초과분 (§10.8)${stageNote}`,
                  warmupSeconds: warmup,
                  tickDigits: 0,
                  yMax: 100,
                  stageMarks: marks,
                  series: [{ label: '거부 %', slot: 2, digits: 1, values: rejectPercent }],
              })
            : emptyChart(
                  '거부율',
                  '응답 중 kEquipResponseUnavailable 의 비율 (%)',
                  '이 JSONL 에는 거부 필드가 없다 — 큐 상한(§10.8)보다 앞선 하네스가 남긴 런이다. ' +
                      '거부율 없는 처리량 곡선은 "상한에서 눌린 것"과 "거부로 흘린 것"을 ' +
                      '구분하지 못한다.',
              ),
        lineChart({
            title: '지연 백분위 — 수락된 요청',
            subtitle:
                '요청 송신 → 응답 수신 (ms) · 보간 없는 nearest-rank · 🔴 거부는 아무것도 ' +
                `실행하지 않고 즉시 답하므로 이 곡선에서 빠져 있다${stageNote}`,
            warmupSeconds: warmup,
            tickDigits: 0,
            stageMarks: marks,
            series: [
                { label: 'p50', slot: 1, digits: 1, values: okP50 },
                { label: 'p99', slot: 2, digits: 1, values: okP99 },
            ],
        }),
        hasProbe
            ? lineChart({
                  title: 'fsync 프로브',
                  subtitle: `DB 볼륨 O_DSYNC 지연 (ms) · §16.1c-① 의 레짐 판별기${stageNote}`,
                  warmupSeconds: warmup,
                  tickDigits: 1,
                  stageMarks: marks,
                  series: [{ label: 'fsync', slot: 1, digits: 2, values: probe }],
              })
            : emptyChart(
                  'fsync 프로브',
                  'DB 볼륨 O_DSYNC 지연 (ms) · §16.1c-① 의 레짐 판별기',
                  '이 런에는 프로브가 없다. 하네스는 프로브를 직접 재지 않고 --probe-file 이 ' +
                      '가리키는 파일을 옮겨 적을 뿐이며, 그 파일은 MySQL 컨테이너 데이터 볼륨에 ' +
                      '대고 도는 외부 루프가 채운다. 프로브 없는 처리량 곡선은 레짐 전환과 ' +
                      '서버 변화를 구분하지 못한다.',
              ),
    ];

    const rows = conditionRows(meta, options.notes, samples)
        .map(
            ([label, value]) =>
                `<tr><th scope="row">${escapeHtml(label)}</th>` +
                `<td>${escapeHtml(value)}</td></tr>`,
        )
        .join('');

    const title = options.title || `atlas_loadgen — ${path.basename(options.input)}`;

    return `<!doctype html>
<html lang="ko">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>${escapeHtml(title)}</title>
<style>
:root {
  color-scheme: light;
  --plane: #f9f9f7;
  --surface: #fcfcfb;
  --ink: #0b0b0b;
  --ink-2: #52514e;
  --muted: #898781;
  --grid: #e1e0d9;
  --axis: #c3c2b7;
  --border: rgba(11, 11, 11, 0.10);
  --wash: rgba(11, 11, 11, 0.04);
  --series-1: ${SERIES[0].light};
  --series-2: ${SERIES[1].light};
}
@media (prefers-color-scheme: dark) {
  :root {
    color-scheme: dark;
    --plane: #0d0d0d;
    --surface: #1a1a19;
    --ink: #ffffff;
    --ink-2: #c3c2b7;
    --muted: #898781;
    --grid: #2c2c2a;
    --axis: #383835;
    --border: rgba(255, 255, 255, 0.10);
    --wash: rgba(255, 255, 255, 0.06);
    --series-1: ${SERIES[0].dark};
    --series-2: ${SERIES[1].dark};
  }
}
* { box-sizing: border-box; }
body {
  margin: 0;
  padding: 32px 20px 56px;
  background: var(--plane);
  color: var(--ink);
  font: 15px/1.6 system-ui, -apple-system, "Segoe UI", sans-serif;
}
main { max-width: 880px; margin: 0 auto; }
h1 { font-size: 22px; margin: 0 0 4px; }
h2 { font-size: 16px; margin: 0; }
p { margin: 0 0 12px; }
.lede { color: var(--ink-2); margin-bottom: 24px; }
section, .chart {
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 16px 18px;
  margin: 0 0 20px;
}
figure.chart { display: block; }
figcaption { margin-bottom: 8px; }
.sub { color: var(--ink-2); font-size: 13px; margin: 2px 0 0; }
.plot { overflow-x: auto; }
svg { display: block; width: 100%; min-width: 560px; height: auto; }
text.ax { fill: var(--muted); font-size: 11px; font-variant-numeric: tabular-nums; }
text.end { fill: var(--ink-2); font-size: 12px; font-variant-numeric: tabular-nums; }
.legend { display: flex; gap: 16px; margin: 8px 0 4px; font-size: 13px; color: var(--ink-2); }
.key { display: inline-flex; align-items: center; gap: 6px; }
.key i { width: 12px; height: 3px; border-radius: 2px; display: inline-block; }
table { border-collapse: collapse; width: 100%; font-size: 14px; }
th, td { text-align: left; padding: 6px 8px; border-bottom: 1px solid var(--border); }
th[scope="row"] { color: var(--ink-2); font-weight: 500; width: 34%; }
td { font-variant-numeric: tabular-nums; }
.empty { color: var(--ink-2); font-size: 13px; margin: 8px 0 0; }
footer { color: var(--muted); font-size: 13px; max-width: 880px; margin: 0 auto; }
</style>
</head>
<body>
<main>
<h1>${escapeHtml(title)}</h1>
<p class="lede">한 런의 초당 시계열이다. 숫자표의 SoT 는 설계 문서 §16.1 이며 이 페이지는 그것을
대체하지 않는다.</p>

<section>
<h2>측정 조건</h2>
<p class="sub">하네스가 스스로 아는 조건만 자동으로 채워진다. 호스트 · 컨테이너 · MySQL 내구성
설정처럼 런이 알 수 없는 조건은 설계 문서 §16.1b 가 SoT 이고, 아래 비고는 그 사본이다.</p>
<table>${rows}</table>
</section>

${charts.join('\n')}
</main>
<footer>생성 ${escapeHtml(new Date().toISOString())} · 입력
${escapeHtml(path.basename(options.input))} · 외부 요청 0 (스크립트 · 폰트 · CDN 없음)</footer>
</body>
</html>
`;
}

function main() {
    const options = parseArgs(process.argv.slice(2));
    const { meta, samples } = readSamples(options.input);
    if (!meta) {
        process.stderr.write('[loadreport] WARN: meta 레코드가 없다 — 측정 조건 표가 비게 된다\n');
    }
    fs.mkdirSync(path.dirname(path.resolve(options.output)), { recursive: true });
    fs.writeFileSync(options.output, buildHtml(options, meta, samples), 'utf8');
    process.stdout.write(
        `[loadreport] ${options.output} (${samples.length} samples, charts=4)\n`,
    );
}

main();
