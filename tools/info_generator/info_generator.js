'use strict';
/**
 * info_generator (C++ 타깃) — `shared/datas/**\/*.csv` → 컴파일-인 정적 데이터 테이블.
 *
 * 세 번째이자 마지막 생성기다(architecture-design.md §14). `pkt_generator` 가 계약을,
 * `db_generator` 가 스키마를 소유하듯 이 생성기는 **기획 데이터**를 소유한다. 경로 · 네임스페이스는
 * 전부 `template.ini [data-gen]` 이 소유한다 — 🔴 하드코딩된 게임/엔진 경로는 없다.
 *
 * 🔴 CSV 1장 = C++ TU 1개. 산출물 구조는 `server/generated/db/` 를 그대로 본뜬다
 *    (`info_meta.{h,cpp}` ↔ `db_meta.{h,cpp}`, `<name>_info.{h,cpp}` ↔ `<table>_row.{h,cpp}`,
 *    `info_sources.cmake` ↔ `db_sources.cmake`). 두 생성기의 출력이 같은 모양이어야 seam 이 하나다.
 *
 * 🔴 이것은 DB 테이블이 아니다. 행은 바이너리에 박히고 런타임에 로드하지 않는다 — 그래서
 *    `db_generator` 와 달리 SQL 도, 커넥션도, 마이그레이션도 없다.
 *
 * 🔴 타입은 `tools/types.json` 의 정규화 타입 키만 허용한다. `int` · `long` 표기가 애초에 존재하지
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
 * 🔴 위치를 가진 오류. `파일:행:열` + 기대값 + 실제값을 전부 담는다 — "어느 CSV 의 몇 행 몇 열이
 * 왜 틀렸는지"를 말하지 않는 데이터 오류는 수백 행짜리 기획 파일 앞에서 무용지물이다.
 * 🔴 throw 하고 스택을 그대로 흘린다. 메시지만 찍고 넘어가면 생성기는 반쪽 출력을 남긴다.
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

function writeTextFile(filePath, content) {
  const full = path.resolve(filePath);
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

// 고정폭 바이트 수. 🔴 이 값은 생성 코드의 static_assert 로 그대로 박힌다 —
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

    // ③ types.json 에 없는 타입. 🔴 `int` · `long` · `varchar` 가 여기서 자동으로 죽는다.
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

  // 🔴 PK 오름차순으로 방출한다 — 조회가 선형 스캔이 아니라 이분 탐색이 되는 이유이며,
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
  '// AUTO-GENERATED by tools/info_generator/info_generator.js — DO NOT EDIT.',
  '// 🔴 Hand edits are silently lost on the next `npm run gen:info` and `npm run gen:info:check`',
  '//    fails the CI gate. Change the CSV under shared/datas/ instead.',
];

const SCOPE_NOTE = [
  '// 🔴 This is COMPILED-IN static data, not a database table (architecture-design.md §3.3, §14).',
  '//    The rows below live in the binary: there is no connection, no query and no load step, and',
  '//    changing a row means re-running the generator and rebuilding. That is the whole point — a',
  '//    lookup that cannot fail for an environmental reason can sit in front of a transaction.',
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
    '//',
    '// Shared vocabulary for the generated info headers: the storage type of a column and the',
    '// compile time column descriptor.',
    '//',
    '// 🔴 The names are Info-prefixed rather than reusing db_meta.h. Both generators emit into one',
    '//    namespace (atlas::generated), so `ColumnType` / `ColumnMeta` would be a redefinition the',
    '//    moment a translation unit includes a row header and an info header — which the equip path',
    '//    does. Making info depend on the db output instead would tie two independent generators',
    '//    together for a struct with four fields.',
    '//',
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
    '// Type of a column as declared in row 3 of the CSV. The enumerators come straight from the',
    '// normalized-type table in tools/types.json, so a new normalized type cannot be used in a CSV',
    '// without appearing here.',
    'enum class InfoColumnType : UInt8 {',
  ];
  Object.keys(cfg.types).map(columnTypeEnumerator).forEach((name, index) => {
    L.push(`    ${name} = ${index},`);
  });
  L.push(
    '};',
    '',
    '// One column of one table. Aggregate on purpose: every instance is a compile time constant in a',
    '// generated header, never something built at run time.',
    'struct InfoColumnMeta {',
    '    std::string_view name_;',
    '    InfoColumnType type_{};',
    '    // Row 2 of the CSV, as the server saw it. `C` columns never reach this array — they are',
    '    // dropped by [data-gen].server_targets — so this only ever says `S` or `CS`.',
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
    '//',
    ...SCOPE_NOTE,
    '',
    '#pragma once',
    '',
  ];
  for (const i of tableHeaderIncludes(table)) L.push(`#include ${i}`);
  L.push('', '#include "atlas/core/types.h"', `#include "${INCLUDE_PREFIX}/info_meta.h"`);
  L.push('', nsOpen(), '');

  L.push(`// One row of ${table.source}, restricted to the columns row 2 marks for the server`);
  L.push(`// ([data-gen].server_targets = ${[...SERVER_TARGETS].join(',')}).`);
  const dropped = table.columns.filter(c => !SERVER_TARGETS.has(c.target));
  if (dropped.length > 0) {
    L.push('// 🔴 Dropped as client-only, and the drop is the seam working rather than data being');
    L.push('//    lost: ' + dropped.map(c => `${c.name} (${c.target})`).join(', ') + '.');
    L.push('//    The client target emits its own language from the SAME csv — the emit language is a');
    L.push('//    property of the target, not of the CSV contract.');
  }
  if (table.kept.some(c => c.cpp === 'std::string_view')) {
    L.push('// ⚠️ Text columns are std::string_view, which cpp-style.md §4.4 otherwise forbids as a');
    L.push('//    member. The rule guards against a view outliving its owner; here the owner is a');
    L.push('//    string literal with static storage duration in this binary, so there is nothing to');
    L.push('//    outlive. Owning std::string would force these rows out of constexpr for nothing.');
  }
  L.push(`struct ${table.typeName} {`);
  for (const c of table.kept) {
    const notes = [];
    if (c.pk) notes.push('primary key');
    if (c.unique) notes.push('unique');
    notes.push(`target ${c.target}`);
    L.push(`    ${c.cpp} ${c.member}{};  // ${notes.join(', ')}`);
  }
  L.push('');
  L.push(`    friend bool operator==(const ${table.typeName}&, const ${table.typeName}&) = default;`);
  L.push('};');
  L.push('');

  const widthColumns = table.kept.filter(c => c.fixedWidth !== null);
  if (widthColumns.length > 0) {
    L.push('// 🔴 Fixed width is asserted, not documented (cpp-style.md §4.1). Windows is LLP64 and Linux');
    L.push('//    is LP64, and this project moves between them on every deploy.');
    for (const c of widthColumns) {
      L.push(`static_assert(sizeof(${table.typeName}::${c.member}) == ${c.fixedWidth}U,`);
      L.push(`              "${table.base}.${c.name} must stay ${c.fixedWidth} bytes wide");`);
    }
    L.push('');
  }

  L.push('// Column metadata, in CSV declaration order.');
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

  L.push('// Every row, in ascending primary-key order.');
  L.push(`[[nodiscard]] std::span<const ${table.typeName}> ${table.rowsFunction}() noexcept;`);
  L.push('');
  L.push(`// The row whose ${table.pk.name} is \`${table.pk.name}\`, or nullptr when this table does not`);
  L.push('// define one (cpp-style.md §4.4 — a non-owning observer, null when there is nothing to point');
  L.push('// at). 🔴 The null case is the server-authority answer to a client naming an id that does not');
  L.push('// exist (architecture-design.md §8.2 layer 3), so it is a value and not an exception.');
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
    '//',
    '// The data itself. 🔴 It lives in the .cpp rather than the header so that one translation unit',
    '//    holds one copy of the table however many places look it up, and so that the unity-OFF build',
    '//    (architecture-design.md §15.1) has a TU here to prove the header is self-contained.',
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
  L.push('// 🔴 Binary search, not a scan: the generator emits the rows sorted by primary key, so this');
  L.push('//    stays O(log n) when a real game brings thousands of rows and the call sites do not change.');
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
    '//',
    '// 🔴 The aggregate header, and it is GENERATED. "Every static table is available in one include"',
    '//    is a claim that rots the moment a CSV is added and a hand-written list is not — so the list',
    '//    is not hand-written.',
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
    '# 🔴 The source list is generated so that adding a CSV never means touching CMake, and so that a',
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

  // 🔴 검증을 전부 끝낸 뒤에 파일을 쓴다 — 뒤쪽 CSV 가 거부당했는데 앞쪽 테이블만 새로 쓰인
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
    '//',
    '// The metadata vocabulary is header-only, so this translation unit exists for one reason: it',
    '// proves the header is self-contained. 🔴 The unity-OFF build is what turns that into a real',
    '//    gate (architecture-design.md §15.1) — a missing include here fails the build instead of',
    '//    being masked by whatever the neighbouring source happened to include first.',
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
  // 🔴 메시지 AND 스택. 스택을 삼키면 생성기 자체의 버그와 데이터 오류를 구분할 수 없다.
  console.error(e.message);
  if (e.stack) console.error(e.stack);
  process.exit(1);
}
