'use strict';
/**
 * info_generator (C++ 타깃) — `shared/datas/**\/*.csv` → 컴파일-인 정적 데이터 테이블.
 *
 * 세 번째이자 마지막 생성기다(architecture-design.md §14). `pkt_generator` 가 계약을,
 * `db_generator` 가 스키마를 소유하듯 이 생성기는 **기획 데이터**를 소유한다. 경로 · 네임스페이스는
 * 전부 `template.ini [data-gen]` 이 소유한다 — 하드코딩된 게임/엔진 경로는 없다.
 *
 * CSV 1장 = C++ TU 1개. 산출물 구조는 `server/generated/db/` 를 그대로 본뜬다
 *    (`info_meta.{h,cpp}` ↔ `db_meta.{h,cpp}`, `<name>_info.{h,cpp}` ↔ `<table>_row.{h,cpp}`,
 *    `info_sources.cmake` ↔ `db_sources.cmake`). 두 생성기의 출력이 같은 모양이어야 seam 이 하나다.
 *
 * 이것은 DB 테이블이 아니다. 행은 바이너리에 박히고 런타임에 로드하지 않는다 — 그래서
 *    `db_generator` 와 달리 SQL 도, 커넥션도, 마이그레이션도 없다.
 *
 * 타입은 `tools/types.json` 의 정규화 타입 키만 허용한다. `int` · `long` 표기가 애초에 존재하지
 *    않으므로 `cpp-style.md §4.1` 금지가 데이터 층에서도 기계적으로 지켜진다.
 *
 * CLI:
 *   --check                    드리프트 검사만(파일을 쓰지 않는다). 어긋나면 exit 1.
 *   --datas-dir=<path>         테스트 전용 입력 경로 오버라이드.
 *   --cpp-output-dir=<path>    테스트 전용 출력 경로 오버라이드.
 */

const fs = require('fs');
const path = require('path');
const cfg = require('../config-loader');
const { formatCxx } = require('../cxx-format');

const CHECK_ONLY = process.argv.includes('--check');

function argValue(flag) {
  const hit = process.argv.find(a => a.startsWith(`${flag}=`));
  return hit ? hit.slice(flag.length + 1) : null;
}

const DATAS_DIR = path.resolve(argValue('--datas-dir') ?? cfg.paths.datasDir);
const CPP_OUTPUT_DIR = path.resolve(argValue('--cpp-output-dir') ?? cfg.dataGen.cppInfoOutputDir);
const CPP_NAMESPACE = cfg.dataGen.cppNamespace;
const SERVER_TARGETS = new Set(cfg.dataGen.serverTargets);
// 생성 헤더의 include 경로. CMake 의 include 루트가 `server/` 이므로(AGENTS.md "Build"),
// 출력 디렉터리를 server/ 기준 상대 경로로 환산한 것이 그대로 include 경로가 된다.
const INCLUDE_PREFIX = path
  .relative(path.join(cfg.root, 'server'), CPP_OUTPUT_DIR)
  .replaceAll(path.sep, '/');

const _state = { changed: [], unchangedCount: 0 };

/**
 * 위치를 가진 오류. `파일:행:열` + 기대값 + 실제값을 전부 담는다 — "어느 CSV 의 몇 행 몇 열이
 * 왜 틀렸는지"를 말하지 않는 데이터 오류는 수백 행짜리 기획 파일 앞에서 무용지물이다.
 * throw 하고 스택을 그대로 흘린다. 메시지만 찍고 넘어가면 생성기는 반쪽 출력을 남긴다.
 */
class InfoError extends Error {
  constructor(where, message) {
    super(`[info] ERROR: ${where}: ${message}`);
    this.name = 'InfoError';
  }
}

function toRel(p) {
  return path.relative(cfg.root, p).replaceAll(path.sep, '/');
}

function ensureDir(dir) {
  if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
}

function writeTextFile(filePath, rawContent) {
  const full = path.resolve(filePath);
  // 생성 C++ 는 저장소 .clang-format 을 통과한 결과만 디스크에 닿는다(tools/cxx-format.js).
  // --check 경로도 같은 결과를 비교해야 gen:check 가 드리프트를 오탐하지 않는다.
  const content = formatCxx(full, rawContent);
  if (fs.existsSync(full) && fs.readFileSync(full, 'utf-8') === content) {
    _state.unchangedCount++;
    return;
  }
  _state.changed.push(toRel(full));
  if (!CHECK_ONLY) {
    ensureDir(path.dirname(full));
    fs.writeFileSync(full, content, 'utf-8');
  }
}

// ── CSV 계약 ─────────────────────────────────────────────────────────────────────
// 5행 헤더(자매 게임 project-tower 와 동일): 1 필드명 / 2 타깃 / 3 정규화 타입 / 4 제약 / 5~ 데이터.
const ROW_FIELD = 1;
const ROW_TARGET = 2;
const ROW_TYPE = 3;
const ROW_CONSTRAINT = 4;
const ROW_FIRST_DATA = 5;

const TARGETS = ['C', 'S', 'CS'];
const CONSTRAINTS = ['PK', 'UQ', 'NN', ''];
const IDENTIFIER = /^[a-z][a-z0-9_]*$/;

// 고정폭 바이트 수. 이 값은 생성 코드의 static_assert 로 그대로 박힌다 —
// 폭이 주석이 아니라 빌드 실패로 강제되는 지점이다(cpp-style.md §4.1).
const FIXED_WIDTHS = {
  int8: 1, uint8: 1, bool: 1,
  int16: 2, uint16: 2,
  int32: 4, uint32: 4, float: 4,
  int64: 8, uint64: 8, double: 8,
};

const INTEGER_RANGES = {
  int8: [-128n, 127n],
  int16: [-32768n, 32767n],
  int32: [-2147483648n, 2147483647n],
  int64: [-9223372036854775808n, 9223372036854775807n],
  uint8: [0n, 255n],
  uint16: [0n, 65535n],
  uint32: [0n, 4294967295n],
  uint64: [0n, 18446744073709551615n],
};

// ── 이름 변환 ────────────────────────────────────────────────────────────────────
function toPascalCase(str) {
  return str.split(/[_\s]/).filter(Boolean).map(s => s.charAt(0).toUpperCase() + s.slice(1)).join('');
}

// cpp-style.md §3 — 멤버는 후행 밑줄.
function member(name) {
  return `${name}_`;
}

// ── 파싱 · 검증 ──────────────────────────────────────────────────────────────────
function listCsvFiles(dir) {
  if (!fs.existsSync(dir)) {
    throw new InfoError(toRel(dir), 'datas directory not found — check [paths].datas_dir');
  }
  const found = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) found.push(...listCsvFiles(full));
    else if (entry.name.toLowerCase().endsWith('.csv')) found.push(full);
  }
  return found;
}

function splitRow(line) {
  return line.split(',').map(cell => cell.trim());
}

/** 셀 수가 필드 행과 다르면 즉사. 오른쪽 끝 빈 칸이 조용히 열을 지우는 것이 이 검사의 목적이다. */
function requireCellCount(where, cells, expected, note) {
  if (cells.length === expected) return;
  throw new InfoError(`${where}:1`,
    `${note}: expected ${expected} cells (one per field in row ${ROW_FIELD}), got ${cells.length}`);
}

function parseValue(where, type, raw, fieldName) {
  const range = INTEGER_RANGES[type];
  if (range !== undefined) {
    if (!/^[+-]?\d+$/.test(raw)) {
      throw new InfoError(where,
        `column "${fieldName}" is ${type}: expected an integer literal, got "${raw}"`);
    }
    const value = BigInt(raw);
    if (value < range[0] || value > range[1]) {
      throw new InfoError(where,
        `column "${fieldName}" is ${type}: expected a value in [${range[0]}, ${range[1]}], got ${value}`);
    }
    return value.toString();
  }
  if (type === 'float' || type === 'double') {
    if (!/^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/.test(raw)) {
      throw new InfoError(where,
        `column "${fieldName}" is ${type}: expected a decimal literal, got "${raw}"`);
    }
    return raw;
  }
  if (type === 'bool') {
    const lowered = raw.toLowerCase();
    if (!['0', '1', 'true', 'false'].includes(lowered)) {
      throw new InfoError(where,
        `column "${fieldName}" is bool: expected one of 0/1/true/false, got "${raw}"`);
    }
    return lowered === '1' || lowered === 'true' ? 'true' : 'false';
  }
  if (type === 'string') {
    if (raw.includes('"') || raw.includes('\\')) {
      throw new InfoError(where,
        `column "${fieldName}" is string: expected no quote or backslash, got "${raw}"`);
    }
    return `"${raw}"`;
  }
  // types.json 에 있으나 여기서 다루지 않는 타입 — 열을 조용히 통과시키지 않는다.
  throw new InfoError(where, `column "${fieldName}" has type "${type}", which this emitter cannot parse`);
}

function analyzeTable(csvPath) {
  const rel = toRel(csvPath);
  const lines = fs.readFileSync(csvPath, 'utf-8').split(/\r?\n/);
  while (lines.length > 0 && lines[lines.length - 1].trim() === '') lines.pop();

  // ① 헤더 5행 미만.
  if (lines.length < ROW_FIRST_DATA) {
    throw new InfoError(`${rel}:${lines.length + 1}:1`,
      `expected the 4-row header (${ROW_FIELD} field / ${ROW_TARGET} target / ${ROW_TYPE} type / ` +
      `${ROW_CONSTRAINT} constraint) followed by at least one data row, got ${lines.length} rows`);
  }

  const fields = splitRow(lines[ROW_FIELD - 1]);
  const targets = splitRow(lines[ROW_TARGET - 1]);
  const types = splitRow(lines[ROW_TYPE - 1]);
  const constraints = splitRow(lines[ROW_CONSTRAINT - 1]);

  // ⑤ 행별 셀 수 불일치 — 헤더 행부터.
  requireCellCount(`${rel}:${ROW_TARGET}`, targets, fields.length, 'target row');
  requireCellCount(`${rel}:${ROW_TYPE}`, types, fields.length, 'type row');
  requireCellCount(`${rel}:${ROW_CONSTRAINT}`, constraints, fields.length, 'constraint row');

  const seenFields = new Set();
  const columns = fields.map((name, index) => {
    const column = index + 1;
    if (!IDENTIFIER.test(name)) {
      throw new InfoError(`${rel}:${ROW_FIELD}:${column}`,
        `field name: expected ${IDENTIFIER}, got "${name}"`);
    }
    if (seenFields.has(name)) {
      throw new InfoError(`${rel}:${ROW_FIELD}:${column}`, `duplicate field name "${name}"`);
    }
    seenFields.add(name);

    // ② 타깃 표기 오타.
    const target = targets[index];
    if (!TARGETS.includes(target)) {
      throw new InfoError(`${rel}:${ROW_TARGET}:${column}`,
        `column "${name}": expected one of ${TARGETS.join(' / ')}, got "${target}"`);
    }

    // ③ types.json 에 없는 타입. `int` · `long` · `varchar` 가 여기서 자동으로 죽는다.
    const type = types[index];
    if (cfg.types[type] === undefined) {
      throw new InfoError(`${rel}:${ROW_TYPE}:${column}`,
        `column "${name}": expected a normalized type from tools/types.json ` +
        `(${Object.keys(cfg.types).join(', ')}), got "${type}"`);
    }

    const constraint = constraints[index];
    if (!CONSTRAINTS.includes(constraint)) {
      throw new InfoError(`${rel}:${ROW_CONSTRAINT}:${column}`,
        `column "${name}": expected one of PK / UQ / NN / (blank), got "${constraint}"`);
    }

    return {
      index,
      column,
      name,
      member: member(name),
      target,
      type,
      cpp: type === 'string' ? 'std::string_view' : cfg.types[type].cpp,
      fixedWidth: FIXED_WIDTHS[type] ?? null,
      pk: constraint === 'PK',
      unique: constraint === 'UQ',
      notNull: constraint === 'NN',
    };
  });

  // ④ PK 열 없음.
  const pkColumns = columns.filter(c => c.pk);
  if (pkColumns.length !== 1) {
    throw new InfoError(`${rel}:${ROW_CONSTRAINT}:1`,
      `expected exactly 1 column marked PK (the lookup key every consumer reaches this table by), ` +
      `got ${pkColumns.length}`);
  }
  const pk = pkColumns[0];
  if (!SERVER_TARGETS.has(pk.target)) {
    throw new InfoError(`${rel}:${ROW_TARGET}:${pk.column}`,
      `PK column "${pk.name}": expected a target the server keeps (${[...SERVER_TARGETS].join(' / ')}), ` +
      `got "${pk.target}" — a server table without its key has no lookup API`);
  }

  const kept = columns.filter(c => SERVER_TARGETS.has(c.target));
  const rows = [];
  const seenKeys = new Map();
  for (let line = ROW_FIRST_DATA; line <= lines.length; line++) {
    const cells = splitRow(lines[line - 1]);
    requireCellCount(`${rel}:${line}`, cells, fields.length, 'data row');

    // ⑥ 타입 파싱 실패 — 버려질 C 전용 열도 검사한다. 클라 emit 이 이 슬라이스 밖이라는 이유로
    // 클라 열의 오타를 통과시키면, 그 오타는 클라 타깃이 붙는 날 한꺼번에 쏟아진다.
    const values = {};
    for (const c of columns) {
      const parsed = parseValue(`${rel}:${line}:${c.column}`, c.type, cells[c.index], c.name);
      if (SERVER_TARGETS.has(c.target)) values[c.name] = parsed;
    }

    // ④ PK 중복값.
    const key = cells[pk.index];
    if (seenKeys.has(key)) {
      throw new InfoError(`${rel}:${line}:${pk.column}`,
        `duplicate ${pk.name} "${key}": expected a unique primary key, but row ${seenKeys.get(key)} ` +
        `already declares it`);
    }
    seenKeys.set(key, line);

    for (const c of kept) {
      if (!c.unique) continue;
      // UQ 는 PK 와 같은 유일성 검사를 비-키 열에 건다.
      const uniqueKey = `${c.name} ${cells[c.index]}`;
      if (seenKeys.has(uniqueKey)) {
        throw new InfoError(`${rel}:${line}:${c.column}`,
          `duplicate ${c.name} "${cells[c.index]}": the column is marked UQ, but row ` +
          `${seenKeys.get(uniqueKey)} already declares it`);
      }
      seenKeys.set(uniqueKey, line);
    }

    rows.push({ line, sortKey: cells[pk.index], values });
  }

  // PK 오름차순으로 방출한다 — 조회가 선형 스캔이 아니라 이분 탐색이 되는 이유이며,
  // 테이블이 수천 행으로 자라도 소비처 코드가 그대로인 이유다.
  const numericPk = INTEGER_RANGES[pk.type] !== undefined;
  rows.sort((a, b) => (numericPk
    ? (BigInt(a.sortKey) < BigInt(b.sortKey) ? -1 : 1)
    : a.sortKey.localeCompare(b.sortKey)));

  const base = path.basename(csvPath, path.extname(csvPath));
  if (!IDENTIFIER.test(base)) {
    throw new InfoError(rel, `file name: expected ${IDENTIFIER} before ".csv", got "${base}"`);
  }
  return {
    source: rel,
    base,
    typeName: `${toPascalCase(base)}InfoRow`,
    fileBase: `${base}_info`,
    constantPrefix: `k${toPascalCase(base)}Info`,
    rowsFunction: `${toPascalCase(base)}InfoRows`,
    findFunction: `Find${toPascalCase(base)}Info`,
    columns,
    kept,
    pk,
    rows,
  };
}

// ── C++ 생성 ─────────────────────────────────────────────────────────────────────
const BANNER = [
  '// =============================================================================',
  '// AUTO-GENERATED by tools/info_generator/info_generator.js - 직접 수정 금지',
  '// 손으로 고친 내용은 다음 gen:info 에서 사라지고 gen:info:check 가 CI 를 막음',
  '// shared/datas/ 아래 CSV 를 고칠 것',
  '// =============================================================================',
];

const SCOPE_NOTE = [
  '// [AD 3.3] [AD 14] DB 테이블이 아니라 바이너리에 박히는 정적 데이터',
  '// 커넥션도 쿼리도 로드 단계도 없다. 행을 바꾸려면 다시 생성하고 빌드한다',
  '// 환경 탓으로 실패할 수 없는 조회라야 트랜잭션 앞에 놓일 수 있다',
];

function nsOpen() {
  return `namespace ${CPP_NAMESPACE} {`;
}

function nsClose() {
  return `}  // namespace ${CPP_NAMESPACE}`;
}

function columnTypeEnumerator(normalized) {
  if (normalized === 'bool') return 'Bool';
  if (normalized === 'string') return 'String';
  return cfg.types[normalized].cpp;
}

function generateMetaHeader() {
  const L = [
    ...BANNER,
    '',
    '// 생성 info 헤더가 공유하는 어휘: 컬럼의 저장 타입과 컴파일 타임 컬럼 서술자',
    '',
    '// db_meta.h 를 재사용하지 않고 Info 접두사를 붙인다',
    '// 두 생성기가 같은 네임스페이스 atlas::generated 로 내보내기 때문',
    '// 행 헤더와 info 헤더를 함께 include 하는 TU 는 장착 경로에 실제로 있다',
    '// 그 TU 가 생기는 순간 ColumnType / ColumnMeta 가 재정의가 된다',
    '',
    ...SCOPE_NOTE,
    '',
    '#pragma once',
    '',
    '#include <cstddef>',
    '#include <string_view>',
    '',
    '#include "atlas/core/types.h"',
    '',
    nsOpen(),
    '',
    '// CSV 3행이 선언한 컬럼 타입',
    '// 열거자는 tools/types.json 정규화 타입 표에서 그대로 온다',
    '// 그래서 표에 없는 타입은 CSV 에도 쓸 수 없다',
    'enum class InfoColumnType : UInt8 {',
  ];
  Object.keys(cfg.types).map(columnTypeEnumerator).forEach((name, index) => {
    L.push(`    ${name} = ${index},`);
  });
  L.push(
    '};',
    '',
    '// 테이블 한 개의 컬럼 한 개. 일부러 aggregate',
    '// 모든 인스턴스는 런타임이 아니라 생성 헤더 안의 컴파일 타임 상수',
    'struct InfoColumnMeta {',
    '    std::string_view name_;',
    '    InfoColumnType type_{};',
    '    // 서버가 본 CSV 2행. 값은 언제나 S 아니면 CS',
    '    // C 전용 열은 [data-gen].server_targets 에서 걸러져 여기 닿지 못한다',
    '    std::string_view target_;',
    '    bool primary_key_{};',
    '    bool unique_{};',
    '',
    '    friend bool operator==(const InfoColumnMeta&, const InfoColumnMeta&) = default;',
    '};',
    '',
    nsClose(),
    '',
  );
  return L.join('\n');
}

function tableHeaderIncludes(table) {
  const std = new Set(['<array>', '<cstddef>', '<span>']);
  for (const c of table.kept) {
    if (c.cpp === 'std::string_view') std.add('<string_view>');
  }
  return [...std].sort();
}

function generateTableHeader(table) {
  const p = table.constantPrefix;
  const L = [
    ...BANNER,
    `// source: ${table.source}`,
    '',
    ...SCOPE_NOTE,
    '',
    '#pragma once',
    '',
  ];
  for (const i of tableHeaderIncludes(table)) L.push(`#include ${i}`);
  L.push('', '#include "atlas/core/types.h"', `#include "${INCLUDE_PREFIX}/info_meta.h"`);
  L.push('', nsOpen(), '');

  L.push(`// ${table.source} 의 한 행. CSV 2행이 서버용으로 표시한 열만 남김`);
  L.push(`// ([data-gen].server_targets = ${[...SERVER_TARGETS].join(',')})`);
  const dropped = table.columns.filter(c => !SERVER_TARGETS.has(c.target));
  if (dropped.length > 0) {
    L.push('// 클라 전용이라 버려진 열: ' + dropped.map(c => `${c.name} (${c.target})`).join(', '));
    L.push('// 데이터가 사라진 것이 아니라 seam 이 동작한 것');
    L.push('// 방출 언어는 타깃의 속성이지 CSV 계약의 속성이 아니다');
  }
  if (table.kept.some(c => c.cpp === 'std::string_view')) {
    L.push('// [CS 4.4] 텍스트 열은 멤버로 금지된 std::string_view 를 쓴다');
    L.push('// 그 금지는 view 가 소유자보다 오래 사는 것을 막기 위한 것');
    L.push('// 여기 소유자는 정적 리터럴이라 수명이 없고 소유하면 constexpr 만 잃는다');
  }
  L.push(`struct ${table.typeName} {`);
  for (const c of table.kept) {
    const notes = [];
    if (c.pk) notes.push('기본 키');
    if (c.unique) notes.push('unique');
    notes.push(`타깃 ${c.target}`);
    L.push(`    ${c.cpp} ${c.member}{};  // ${notes.join(', ')}`);
  }
  L.push('');
  L.push(`    friend bool operator==(const ${table.typeName}&, const ${table.typeName}&) = default;`);
  L.push('};');
  L.push('');

  const widthColumns = table.kept.filter(c => c.fixedWidth !== null);
  if (widthColumns.length > 0) {
    L.push('// [CS 4.1] 고정폭은 문서가 아니라 단언으로 강제한다');
    L.push('// Windows 는 LLP64, Linux 는 LP64 이고 배포마다 둘 사이를 오간다');
    for (const c of widthColumns) {
      L.push(`static_assert(sizeof(${table.typeName}::${c.member}) == ${c.fixedWidth}U,`);
      L.push(`              "${table.base}.${c.name} must stay ${c.fixedWidth} bytes wide");`);
    }
    L.push('');
  }

  L.push('// CSV 선언 순서대로의 컬럼 메타데이터');
  L.push(`inline constexpr std::string_view ${p}Table = "${table.base}";`);
  L.push(`inline constexpr std::size_t ${p}ColumnCount = ${table.kept.length};`);
  L.push(`inline constexpr std::size_t ${p}RowCount = ${table.rows.length};`);
  L.push('');
  table.kept.forEach((c, index) => {
    L.push(`inline constexpr std::size_t ${p}Col${toPascalCase(c.name)} = ${index};`);
  });
  L.push('');
  L.push(`inline constexpr std::array<InfoColumnMeta, ${p}ColumnCount> ${p}Columns = {{`);
  for (const c of table.kept) {
    L.push(
      `    {.name_ = "${c.name}",` +
      ` .type_ = InfoColumnType::${columnTypeEnumerator(c.type)},` +
      ` .target_ = "${c.target}",` +
      ` .primary_key_ = ${c.pk},` +
      ` .unique_ = ${c.unique}},`);
  }
  L.push('}};');
  L.push('');

  L.push('// 기본 키 오름차순의 전체 행');
  L.push(`[[nodiscard]] std::span<const ${table.typeName}> ${table.rowsFunction}() noexcept;`);
  L.push('');
  L.push(`// [CS 4.4] ${table.pk.name} 이 일치하는 행, 없으면 nullptr`);
  L.push('// 가리킬 것이 없을 때 null 이 되는 비소유 관찰자');
  L.push('// [AD 8.2] null 은 없는 id 를 댄 클라에 대한 서버 권위의 답이라 예외가 아니다');
  L.push(`[[nodiscard]] const ${table.typeName}* ${table.findFunction}(${table.pk.cpp} ${table.pk.name}) noexcept;`);
  L.push('');
  L.push(nsClose(), '');
  return L.join('\n');
}

function generateTableSource(table) {
  const p = table.constantPrefix;
  const L = [
    ...BANNER,
    `// source: ${table.source}`,
    '',
    '// [AD 15.1] 데이터 본체. 헤더가 아니라 .cpp 에 둔다',
    '// 조회처가 몇 곳이든 테이블 사본은 TU 하나에만 생긴다',
    '// unity OFF 빌드가 헤더의 자기 완결을 증명할 TU 도 이렇게 생긴다',
    '',
    `#include "${INCLUDE_PREFIX}/${table.fileBase}.h"`,
    '',
    '#include <algorithm>',
    '#include <array>',
    '#include <span>',
    '',
    nsOpen(),
    '',
    'namespace {',
    '',
    `constexpr std::array<${table.typeName}, ${p}RowCount> kRows = {{`,
  ];
  for (const row of table.rows) {
    const fields = table.kept.map(c => `.${c.member} = ${row.values[c.name]}`);
    L.push(`    {${fields.join(', ')}},`);
  }
  L.push('}};');
  L.push('');
  L.push('}  // namespace');
  L.push('');
  L.push(`std::span<const ${table.typeName}> ${table.rowsFunction}() noexcept { return kRows; }`);
  L.push('');
  L.push('// 선형 스캔이 아니라 이분 탐색. 생성기가 행을 기본 키로 정렬해 내보낸다');
  L.push('// 실제 게임이 수천 행을 가져와도 O(log n) 이고 호출처는 그대로다');
  L.push(`const ${table.typeName}* ${table.findFunction}(${table.pk.cpp} ${table.pk.name}) noexcept {`);
  L.push('    const auto hit = std::lower_bound(');
  L.push(`        kRows.begin(), kRows.end(), ${table.pk.name},`);
  L.push(`        [](const ${table.typeName}& row, ${table.pk.cpp} key) { return row.${table.pk.member} < key; });`);
  L.push(`    if (hit == kRows.end() || hit->${table.pk.member} != ${table.pk.name}) {`);
  L.push('        return nullptr;');
  L.push('    }');
  L.push('    return &*hit;');
  L.push('}');
  L.push('');
  L.push(nsClose(), '');
  return L.join('\n');
}

function generateAllHeader(tables) {
  return [
    ...BANNER,
    '',
    '// 집합 헤더이고 생성물이다',
    '// "정적 테이블 전부가 include 하나로 들어온다"는 주장은 손으로 쓰면 썩는다',
    '// CSV 가 추가되는 순간 목록이 뒤처지기 때문',
    '',
    '#pragma once',
    '',
    `#include "${INCLUDE_PREFIX}/info_meta.h"`,
    ...tables.map(t => `#include "${INCLUDE_PREFIX}/${t.fileBase}.h"`),
    '',
  ].join('\n');
}

function generateSourcesCMake(sourceFiles) {
  return [
    '# AUTO-GENERATED by tools/info_generator/info_generator.js — DO NOT EDIT.',
    '# The source list is generated so that adding a CSV never means touching CMake, and so that a',
    '#    stale list cannot silently drop a table from the build. CMakeLists.txt includes this.',
    'set(ATLAS_GENERATED_INFO_SOURCES',
    ...sourceFiles.map(f => `    ${f}`),
    ')',
    '',
  ].join('\n');
}

// ── Main ──────────────────────────────────────────────────────────────────────
function main() {
  const csvFiles = listCsvFiles(DATAS_DIR);
  if (csvFiles.length === 0) {
    throw new InfoError(toRel(DATAS_DIR),
      'no *.csv found — an empty output directory would silently drop every static table from the ' +
      'build, so this is an error rather than a no-op');
  }

  // 검증을 전부 끝낸 뒤에 파일을 쓴다 — 뒤쪽 CSV 가 거부당했는데 앞쪽 테이블만 새로 쓰인
  //    반쪽 출력이 남으면 안 된다(db_generator · pkt_generator 와 같은 규약).
  const tables = csvFiles.map(analyzeTable);
  const bases = new Set();
  for (const t of tables) {
    if (bases.has(t.base)) {
      throw new InfoError(t.source, `duplicate table name "${t.base}" — one CSV basename, one C++ TU`);
    }
    bases.add(t.base);
  }

  const sourceFiles = ['info_meta.cpp'];
  writeTextFile(path.join(CPP_OUTPUT_DIR, 'info_meta.h'), generateMetaHeader());
  writeTextFile(path.join(CPP_OUTPUT_DIR, 'info_meta.cpp'), [
    ...BANNER,
    '',
    '// 메타데이터 어휘는 헤더 온리. 이 TU 는 헤더가 자기 완결임을 증명',
    '// [AD 15.1] 그 증명을 실제 게이트로 만드는 것은 unity OFF 빌드',
    '// 여기서 빠진 include 는 이웃에 가려지지 않고 빌드를 깨뜨린다',
    '',
    `#include "${INCLUDE_PREFIX}/info_meta.h"`,
    '',
  ].join('\n'));

  for (const table of tables) {
    writeTextFile(path.join(CPP_OUTPUT_DIR, `${table.fileBase}.h`), generateTableHeader(table));
    writeTextFile(path.join(CPP_OUTPUT_DIR, `${table.fileBase}.cpp`), generateTableSource(table));
    sourceFiles.push(`${table.fileBase}.cpp`);
  }
  writeTextFile(path.join(CPP_OUTPUT_DIR, 'info_all.h'), generateAllHeader(tables));
  writeTextFile(path.join(CPP_OUTPUT_DIR, 'info_sources.cmake'), generateSourcesCMake(sourceFiles));

  if (CHECK_ONLY && _state.changed.length > 0) {
    console.error(`[info] ERROR: generated info artifacts are out of date (changed=${_state.changed.length}).`);
    for (const f of _state.changed) console.error(`[info]        ${f}`);
    console.error('[info]        Run `npm run gen:info` and commit the result.');
    process.exit(1);
  }

  const rowCount = tables.reduce((sum, t) => sum + t.rows.length, 0);
  const keptCount = tables.reduce((sum, t) => sum + t.kept.length, 0);
  console.log(`[info] tables=${tables.length} rows=${rowCount} server-columns=${keptCount}`);
  console.log(`[info] output: ${toRel(CPP_OUTPUT_DIR)} (namespace ${CPP_NAMESPACE})`);
  console.log(`[info] files: changed=${_state.changed.length} unchanged=${_state.unchangedCount}`);
}

try {
  main();
} catch (e) {
  // 메시지 AND 스택. 스택을 삼키면 생성기 자체의 버그와 데이터 오류를 구분할 수 없다.
  console.error(e.message);
  if (e.stack) console.error(e.stack);
  process.exit(1);
}
