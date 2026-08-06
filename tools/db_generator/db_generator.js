'use strict';
/**
 * db_generator (C++ 타깃) — `server/db/schema.json` → MySQL `schema.sql` + C++ 행 구조체 / 컬럼
 * 메타데이터 / prepared statement 상수 / 바인딩 순서 배열.
 *
 * 스키마 한 벌에서 여러 타깃을 뽑는 것이 이 프레임워크의 seam 이다(architecture-design.md §14).
 * SQL 방출 로직은 자매 프로젝트(project-tower)의 db_generator 에서 그대로 가져왔고, 이 파일은
 * 거기에 **C++ emitter 를 추가**한 것이다. 경로 · 네임스페이스는 전부 `template.ini [db-gen]` 이
 * 소유한다 — 🔴 하드코딩된 게임/엔진 경로는 없다.
 *
 * 🔴 이 슬라이스의 경계 — **ORM 런타임을 만들지 않는다**(architecture-design.md §10).
 *    §10 의 ORM 은 "생성물 + 런타임" 두 부분이고, 런타임(커넥션 풀 · 트랜잭션 RAII ·
 *    per-character lock)은 아직 없다. 런타임이 없는데 CRUD 실행 코드를 뽑으면 컴파일되지 않는다.
 *    여기서 나오는 것: 행 구조체 · 컬럼 메타데이터 · placeholder SQL 상수 · 바인딩 순서 배열.
 *    여기서 나오지 않는 것: 실행 API · 커넥션 · 트랜잭션 · 락. 그것은 ORM 런타임 노드의 몫이고,
 *    그 노드가 생기면 이 emitter 를 확장해 CRUD 함수 시그니처까지 뽑는다.
 *
 * 🔴 원본의 **라이브 DB 마이그레이션 diff 는 이식하지 않았다.** 실 DB 가 없고, 공개 레포에 접속
 *    자격 증명이 얽히면 안 된다. 남은 것은 원본 `--generate-only` 상당의 **오프라인 SQL 파일
 *    출력**뿐이다. `template.ini [db-gen]` 에 접속 키가 없는 것도 같은 이유다.
 *
 * 🔴 문자열 SQL 조립 금지(architecture-design.md §10). 생성되는 SQL 은 `?` placeholder 가 박힌
 *    **고정 문자열 상수**이고, 컬럼명·값을 런타임에 이어 붙이는 코드는 뽑지 않는다.
 *
 * CLI:
 *   --check                    드리프트 검사만(파일을 쓰지 않는다). 어긋나면 exit 1.
 *   --schema=<path>            테스트 전용 입력 경로 오버라이드.
 *   --cpp-output-dir=<path>    테스트 전용 C++ 출력 경로 오버라이드.
 *   --sql-output=<path>        테스트 전용 SQL 출력 경로 오버라이드.
 */

const fs = require('fs');
const path = require('path');
const cfg = require('../config-loader');

const CHECK_ONLY = process.argv.includes('--check');

function argValue(flag) {
  const hit = process.argv.find(a => a.startsWith(`${flag}=`));
  return hit ? hit.slice(flag.length + 1) : null;
}

const SCHEMA_PATH = path.resolve(argValue('--schema') ?? cfg.dbGen.schemaPath);
const CPP_OUTPUT_DIR = path.resolve(argValue('--cpp-output-dir') ?? cfg.dbGen.cppDbOutputDir);
const SQL_OUTPUT = path.resolve(argValue('--sql-output') ?? cfg.dbGen.sqlOutput);
const CPP_NAMESPACE = cfg.dbGen.cppNamespace;
// 생성 헤더의 include 경로. CMake 의 include 루트가 `server/` 이므로(AGENTS.md "Build"),
// 출력 디렉터리를 server/ 기준 상대 경로로 환산한 것이 그대로 include 경로가 된다.
const INCLUDE_PREFIX = path
  .relative(path.join(cfg.root, 'server'), CPP_OUTPUT_DIR)
  .replaceAll(path.sep, '/');

const _state = { changed: [], unchangedCount: 0 };

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

function fail(lines) {
  for (const line of lines) console.error(line);
  process.exit(1);
}

// ── 타입 해석 ────────────────────────────────────────────────────────────────────
// 🔴 정규화 타입 → C++ / MySQL 매핑의 SoT 는 `tools/types.json` 이다. 여기에 표를 복제하지 않는다.
// `string(N)` · `datetime` · `json` 만 표 밖의 특수 처리이며, 그 세 개는 아래 한 곳에서 다룬다.
const SPECIAL_TYPES = {
  datetime: { cppScalar: 'SysTime', columnType: 'DateTime', mysql: 'DATETIME', include: 'atlas/core/time.h' },
  json: { cppScalar: 'std::string', columnType: 'Json', mysql: 'JSON', include: null },
};

function parseStringN(type) {
  const m = /^string\((\d+)\)$/.exec(type);
  return m ? parseInt(m[1], 10) : null;
}

// 고정폭 바이트 수. 🔴 이 값은 생성 코드의 static_assert 로 그대로 박힌다 —
// 폭이 주석이 아니라 빌드 실패로 강제되는 지점이다(cpp-style.md §4.1).
const FIXED_WIDTHS = {
  int8: 1, uint8: 1, bool: 1,
  int16: 2, uint16: 2,
  int32: 4, uint32: 4, float: 4,
  int64: 8, uint64: 8, double: 8,
};

// 🔴 컬럼 이름 → 강타입 ID 매핑표(cpp-style.md §4.3, server/atlas/core/ids.h).
//    전부 `UInt64` 별칭으로 두면 `Load(server_id, character_id)` 에 인자 순서를 바꿔 넣어도
//    컴파일이 통과하고 런타임에 조용히 틀린다. `enum class` 는 암묵 변환이 없어 같은 실수가
//    컴파일 에러가 되며 런타임 비용은 0이다.
//    ⚠️ `server_id` 는 일부러 여기 없다 — `ids.h` 가 "인자로 돌아다니지 않는 값에 강타입을 주면
//    얻는 것이 없다"는 이유로 아직 약타입으로 두었고, 이 표가 그 결정을 앞질러 가지 않는다.
//    `account_uid` 는 platform-auth 가 소유하는 계정 식별자(design §6)이며 이름만 uid 일 뿐
//    같은 계정 아이덴티티라 `AccountId` 로 뽑는다.
const ID_COLUMNS = new Map([
  ['account_id', { cpp: 'AccountId', normalized: 'uint64' }],
  ['account_uid', { cpp: 'AccountId', normalized: 'uint64' }],
  ['character_id', { cpp: 'CharacterId', normalized: 'uint64' }],
  ['session_id', { cpp: 'SessionId', normalized: 'uint64' }],
  ['actor_id', { cpp: 'ActorId', normalized: 'uint64' }],
]);

// 메타데이터의 `ColumnType` 열거자 이름. 스칼라는 types.json 의 `cpp` 별칭을 그대로 쓰고,
// `bool` / `string` 만 PascalCase 로 맞춘다(cpp-style.md §3 — enum 값은 PascalCase).
function columnTypeEnumerator(normalized) {
  if (normalized === 'bool') return 'Bool';
  if (normalized === 'string') return 'String';
  return cfg.types[normalized].cpp;
}

function columnTypeEnumerators() {
  const names = Object.keys(cfg.types).map(columnTypeEnumerator);
  for (const special of Object.values(SPECIAL_TYPES)) names.push(special.columnType);
  return names;
}

function whereOf(table, column) {
  return `${toRel(SCHEMA_PATH)}: ${table}.${column}`;
}

/** schema.json 의 `type` 문자열 → 타입 노드. 지원하지 않는 타입은 여기서 즉사시킨다. */
function resolveType(table, col) {
  const declared = col.type;
  const stringN = parseStringN(declared);
  if (stringN !== null) {
    return {
      cpp: 'std::string',
      columnType: 'String',
      mysql: `VARCHAR(${stringN})`,
      maxLength: stringN,
      include: null,
      fixedWidth: null,
    };
  }

  const special = SPECIAL_TYPES[declared];
  if (special !== undefined) {
    return {
      cpp: special.cppScalar,
      columnType: special.columnType,
      mysql: special.mysql,
      maxLength: 0,
      include: special.include,
      fixedWidth: null,
    };
  }

  const row = cfg.types[declared];
  if (row === undefined) {
    fail([
      `[db] ERROR: ${whereOf(table, col.name)}: type "${declared}" has no mapping.`,
      '[db]        Known types come from tools/types.json (the normalized-type SoT) plus the three',
      `[db]        special forms handled here: string(N), ${Object.keys(SPECIAL_TYPES).join(', ')}.`,
      '[db]        Add the type to types.json (all 4 columns) or change the column.',
    ]);
  }
  if (!row.mysql) {
    fail([
      `[db] ERROR: ${whereOf(table, col.name)}: type "${declared}" has no "mysql" column in`,
      '[db]        tools/types.json, so no CREATE TABLE statement can be emitted for it.',
    ]);
  }

  const idMapping = ID_COLUMNS.get(col.name);
  if (idMapping !== undefined && idMapping.normalized !== declared) {
    fail([
      `[db] ERROR: ${whereOf(table, col.name)}: identifier column is declared "${declared}" but`,
      `[db]        ${idMapping.cpp} is built on ${idMapping.normalized} (server/atlas/core/ids.h).`,
      '[db]        A narrower storage width than the strong-typed ID silently truncates identifiers',
      '[db]        that the server hands out (cpp-style.md 4.3). Fix the schema type.',
    ]);
  }

  return {
    cpp: idMapping !== undefined ? idMapping.cpp : row.cpp,
    columnType: columnTypeEnumerator(declared),
    mysql: row.mysql,
    maxLength: 0,
    include: idMapping !== undefined ? 'atlas/core/ids.h' : null,
    // 고정폭 정수/실수만 폭 검사를 붙인다. 나머지(문자열 · 시각 · nullable)는 폭이 규정 대상이 아니다.
    fixedWidth: FIXED_WIDTHS[declared] ?? null,
  };
}

// ── 이름 변환 ────────────────────────────────────────────────────────────────────
function toPascalCase(str) {
  return str.split(/[_\s]/).filter(Boolean).map(s => s.charAt(0).toUpperCase() + s.slice(1)).join('');
}

// cpp-style.md §3 — 멤버는 후행 밑줄.
function member(name) {
  return `${name}_`;
}

// ── 스키마 검증 ──────────────────────────────────────────────────────────────────
const IDENTIFIER = /^[a-z][a-z0-9_]*$/;

function loadSchema() {
  if (!fs.existsSync(SCHEMA_PATH)) {
    fail([
      `[db] ERROR: schema not found: ${toRel(SCHEMA_PATH)}.`,
      '[db]        db_generator has nothing to emit, and an empty output directory would silently',
      '[db]        drop every row struct from the build. Fix [db-gen].schema in template.ini.',
    ]);
  }
  let schema;
  try {
    schema = JSON.parse(fs.readFileSync(SCHEMA_PATH, 'utf-8'));
  } catch (e) {
    fail([`[db] ERROR: ${toRel(SCHEMA_PATH)} is not valid JSON: ${e.message}`]);
  }
  if (!Array.isArray(schema.tables) || schema.tables.length === 0) {
    fail([`[db] ERROR: ${toRel(SCHEMA_PATH)}: "tables" must be a non-empty array.`]);
  }
  if (typeof schema.database !== 'string' || schema.database === '') {
    fail([`[db] ERROR: ${toRel(SCHEMA_PATH)}: "database" must be a non-empty string.`]);
  }
  return schema;
}

/**
 * 🔴 검증을 전부 끝낸 뒤에 파일을 쓴다 — 뒤쪽 테이블이 거부당했는데 앞쪽 구조체만 새로 쓰인
 * 반쪽 출력이 남으면 안 된다(pkt_generator 와 같은 규약).
 */
function analyze(schema) {
  const tableNames = new Set();
  const tables = [];

  for (const table of schema.tables) {
    if (typeof table.name !== 'string' || !IDENTIFIER.test(table.name)) {
      fail([`[db] ERROR: ${toRel(SCHEMA_PATH)}: table name "${table.name}" must match ${IDENTIFIER}.`]);
    }
    if (tableNames.has(table.name)) {
      fail([`[db] ERROR: ${toRel(SCHEMA_PATH)}: duplicate table "${table.name}".`]);
    }
    tableNames.add(table.name);

    if (!Array.isArray(table.columns) || table.columns.length === 0) {
      fail([`[db] ERROR: ${toRel(SCHEMA_PATH)}: table "${table.name}" has no columns.`]);
    }

    const columnNames = new Set();
    const columns = table.columns.map((col, index) => {
      if (typeof col.name !== 'string' || !IDENTIFIER.test(col.name)) {
        fail([`[db] ERROR: ${toRel(SCHEMA_PATH)}: column name "${col.name}" in table "${table.name}" must match ${IDENTIFIER}.`]);
      }
      if (columnNames.has(col.name)) {
        fail([`[db] ERROR: ${whereOf(table.name, col.name)}: duplicate column.`]);
      }
      columnNames.add(col.name);

      const resolved = resolveType(table.name, col);
      // pk 는 NOT NULL 을 함의한다 — 원본 db_generator 와 같은 규약.
      const nullable = col.pk !== true && col.null === true;
      return {
        index,
        name: col.name,
        member: member(col.name),
        pk: col.pk === true,
        nullable,
        auto: col.auto === true,
        unique: col.unique === true,
        default: col.default,
        fk: col.fk,
        ...resolved,
      };
    });

    const pkColumns = columns.filter(c => c.pk);
    if (pkColumns.length === 0) {
      fail([
        `[db] ERROR: ${toRel(SCHEMA_PATH)}: table "${table.name}" declares no primary key.`,
        '[db]        Every persisted table needs one: the by-PK prepared statements are generated',
        '[db]        from it, and the per-character lock discipline (architecture-design.md 6, 10)',
        '[db]        is keyed on it. Mark the key column(s) with "pk": true.',
      ]);
    }

    for (const idx of table.indexes ?? []) {
      for (const c of idx.columns ?? []) {
        if (!columnNames.has(c)) {
          fail([`[db] ERROR: ${toRel(SCHEMA_PATH)}: index "${idx.name}" on table "${table.name}" references unknown column "${c}".`]);
        }
      }
    }

    tables.push({
      name: table.name,
      comment: table.comment,
      typeName: `${toPascalCase(table.name)}Row`,
      fileBase: `${table.name}_row`,
      constantPrefix: `k${toPascalCase(table.name)}`,
      columns,
      pkColumns,
      nonPkColumns: columns.filter(c => !c.pk),
      indexes: table.indexes ?? [],
      raw: table,
    });
  }

  tables.sort((a, b) => a.fileBase.localeCompare(b.fileBase));
  return tables;
}

// ── SQL 생성 (원본 db_generator 이식) ────────────────────────────────────────────
function quoted(name) {
  return '`' + name + '`';
}

function sqlDefault(col) {
  if (col.default === undefined) return '';
  if (typeof col.default === 'boolean') return ` DEFAULT ${col.default ? 1 : 0}`;
  if (typeof col.default === 'number') return ` DEFAULT ${col.default}`;
  return ` DEFAULT '${col.default}'`;
}

function generateTableSql(table) {
  const lines = [];
  if (table.comment) lines.push(`-- ${table.comment}`);
  lines.push(`CREATE TABLE IF NOT EXISTS ${quoted(table.name)} (`);

  const defs = [];
  for (const c of table.columns) {
    let def = `  ${quoted(c.name)} ${c.mysql}`;
    def += c.nullable ? ' NULL' : ' NOT NULL';
    if (c.auto) def += ' AUTO_INCREMENT';
    def += sqlDefault(c);
    defs.push(def);
  }
  defs.push(`  PRIMARY KEY (${table.pkColumns.map(c => quoted(c.name)).join(', ')})`);
  for (const c of table.columns) {
    if (c.unique) defs.push(`  UNIQUE KEY ${quoted(`uq_${table.name}_${c.name}`)} (${quoted(c.name)})`);
  }
  for (const c of table.columns) {
    if (!c.fk) continue;
    const [refTable, refColumn] = c.fk.split('.');
    defs.push(
      `  CONSTRAINT ${quoted(`fk_${table.name}_${c.name}`)} FOREIGN KEY (${quoted(c.name)}) ` +
      `REFERENCES ${quoted(refTable)} (${quoted(refColumn)})`);
  }
  for (const idx of table.indexes) {
    const kind = idx.unique ? 'UNIQUE KEY' : 'KEY';
    defs.push(`  ${kind} ${quoted(idx.name)} (${idx.columns.map(quoted).join(', ')})`);
  }

  lines.push(defs.join(',\n'));
  lines.push(') ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;');
  return lines.join('\n');
}

function generateSql(schema, tables) {
  const parts = [
    '-- AUTO-GENERATED by tools/db_generator/db_generator.js — DO NOT EDIT.',
    '-- 🔴 Hand edits are silently lost on the next `npm run gen:db` and `npm run gen:db:check`',
    '--    fails the CI gate. Change server/db/schema.json instead.',
    '--',
    '-- 🔴 This is an OFFLINE script, not a migration. db_generator never connects to a database:',
    '--    a live ALTER diff would drag connection settings into a public repository and there is no',
    '--    server to diff against yet (architecture-design.md 10). Apply this by hand.',
    `-- database: ${schema.database}`,
    '',
  ];
  for (const t of tables) {
    parts.push(generateTableSql(t));
    parts.push('');
  }
  return parts.join('\n');
}

// ── C++ 생성 ─────────────────────────────────────────────────────────────────────
const BANNER = [
  '// AUTO-GENERATED by tools/db_generator/db_generator.js — DO NOT EDIT.',
  '// 🔴 Hand edits are silently lost on the next `npm run gen:db` and `npm run gen:db:check`',
  '//    fails the CI gate. Change server/db/schema.json instead.',
];

const SCOPE_NOTE = [
  '// 🔴 Scope of this slice (architecture-design.md 10): row structs, column metadata, prepared',
  '//    statement text and binding order — and nothing that executes. There is no connection, no',
  '//    transaction scope and no per-character lock here; those belong to the ORM runtime and land',
  '//    in a later slice, which will extend this generator with the CRUD signatures on top of what',
  '//    is emitted here. Generating call code against a runtime that does not exist would not even',
  '//    compile.',
  '// 🔴 No SQL is assembled at run time. Every statement below is a fixed constant with `?`',
  '//    placeholders, so a value can never be spliced into the statement text.',
];

function nsOpen() {
  return `namespace ${CPP_NAMESPACE} {`;
}

function nsClose() {
  return `}  // namespace ${CPP_NAMESPACE}`;
}

function generateMetaHeader() {
  const L = [
    ...BANNER,
    '//',
    '// Shared vocabulary for the generated row headers: the storage type of a column, the compile',
    '// time column descriptor, and the placeholder counter that ties each statement to its binding',
    '// order.',
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
    '// Storage type of a column as declared in server/db/schema.json.',
    '// 🔴 This describes the DATABASE column, not the C++ field. An identifier column is stored as',
    '//    UInt64 but the row struct carries a strong-typed ID (cpp-style.md 4.3), so the two are',
    '//    deliberately allowed to differ.',
    '// 🔴 The underlying width is spelled out, and the enumerators come straight from the',
    '//    normalized-type table in tools/types.json, so a new normalized type cannot be used in a',
    '//    schema without appearing here.',
    'enum class ColumnType : UInt8 {',
  ];
  columnTypeEnumerators().forEach((name, index) => L.push(`    ${name} = ${index},`));
  L.push(
    '};',
    '',
    '// One column of one table. Aggregate on purpose: every instance is a compile time constant in a',
    '// generated header, never something built at run time.',
    'struct ColumnMeta {',
    '    std::string_view name_;',
    '    ColumnType type_{};',
    '    bool primary_key_{};',
    '    bool nullable_{};',
    '    // string(N) carries N here so a length check has a compile time bound; every other type',
    '    // leaves it at 0.',
    '    std::size_t max_length_{};',
    '',
    '    friend bool operator==(const ColumnMeta&, const ColumnMeta&) = default;',
    '};',
    '',
    '// Number of `?` placeholders in a prepared statement.',
    '// 🔴 This exists so that "the binding array matches the statement" is a static_assert in every',
    '//    generated header rather than a promise in a comment: a statement and its binding order',
    '//    that drift apart fail the build instead of misbinding a parameter at run time.',
    'constexpr std::size_t CountPlaceholders(std::string_view sql) {',
    '    std::size_t count = 0;',
    '    for (const char c : sql) {',
    "        if (c == '?') ++count;",
    '    }',
    '    return count;',
    '}',
    '',
    nsClose(),
    '',
  );
  return L.join('\n');
}

function selfContainedSource(headerName, note) {
  return [
    ...BANNER,
    '//',
    `// ${note}`,
    '// 🔴 The unity-OFF build is what turns that into a real gate (architecture-design.md 15.1) — a',
    '//    missing include here fails the build instead of being masked by whatever the neighbouring',
    '//    source happened to include first.',
    '',
    `#include "${INCLUDE_PREFIX}/${headerName}"`,
    '',
  ].join('\n');
}

function cppFieldType(col) {
  return col.nullable ? `std::optional<${col.cpp}>` : col.cpp;
}

// 스키마의 default 를 멤버 초기자로 옮긴다 — 기본값이 DB 와 C++ 두 곳에서 갈리지 않게.
// nullable 컬럼은 제외한다: "값 없음"과 "기본값"은 다른 상태이고, optional 의 기본은 전자다.
function memberInitializer(col) {
  if (col.nullable || col.default === undefined) return '{}';
  if (typeof col.default === 'boolean') return `{${col.default}}`;
  if (typeof col.default === 'number') return `{${col.default}}`;
  if (col.cpp === 'std::string') return `{"${col.default}"}`;
  return '{}';
}

function rowHeaderIncludes(table) {
  const std = new Set(['<array>', '<cstddef>', '<string_view>']);
  const atlas = new Set(['atlas/core/types.h']);
  for (const c of table.columns) {
    if (c.nullable) std.add('<optional>');
    if (c.cpp === 'std::string') std.add('<string>');
    if (c.include) atlas.add(c.include);
  }
  return {
    std: [...std].sort(),
    atlas: [...atlas].sort(),
  };
}

function statementsOf(table) {
  const all = table.columns.map(c => quoted(c.name)).join(', ');
  const pkPredicate = table.pkColumns.map(c => `${quoted(c.name)} = ?`).join(' AND ');
  const statements = [
    {
      suffix: 'SelectByPk',
      note: ['Reads one row by primary key.'],
      sql: `SELECT ${all} FROM ${quoted(table.name)} WHERE ${pkPredicate}`,
      binding: table.pkColumns,
    },
    {
      suffix: 'Insert',
      note: ['Inserts one full row. Every column is bound, in declaration order.'],
      sql: `INSERT INTO ${quoted(table.name)} (${all}) ` +
        `VALUES (${table.columns.map(() => '?').join(', ')})`,
      binding: table.columns,
    },
  ];
  if (table.nonPkColumns.length > 0) {
    statements.push({
      suffix: 'UpdateByPk',
      note: [
        'Overwrites every non-key column of one row.',
        '🔴 The key columns bind LAST, after the SET list. That ordering is the whole reason the',
        '   binding array is generated instead of being assumed to match the column order.',
      ],
      sql: `UPDATE ${quoted(table.name)} SET ` +
        `${table.nonPkColumns.map(c => `${quoted(c.name)} = ?`).join(', ')} WHERE ${pkPredicate}`,
      binding: [...table.nonPkColumns, ...table.pkColumns],
    });
  }
  statements.push({
    suffix: 'DeleteByPk',
    note: ['Deletes one row by primary key.'],
    sql: `DELETE FROM ${quoted(table.name)} WHERE ${pkPredicate}`,
    binding: table.pkColumns,
  });
  return statements;
}

function generateRowHeader(table) {
  const p = table.constantPrefix;
  const includes = rowHeaderIncludes(table);

  const L = [
    ...BANNER,
    `// source: ${toRel(SCHEMA_PATH)} (table \`${table.name}\`)`,
  ];
  if (table.comment) L.push('//', `// ${table.comment}`);
  L.push('//', ...SCOPE_NOTE, '', '#pragma once', '');
  for (const i of includes.std) L.push(`#include ${i}`);
  L.push('');
  for (const i of includes.atlas) L.push(`#include "${i}"`);
  L.push(`#include "${INCLUDE_PREFIX}/db_meta.h"`);
  L.push('', nsOpen(), '');

  // ── row struct ──
  L.push('// One row of the table. Plain aggregate: no change tracking, no lazy loading and no unit of');
  L.push('// work (architecture-design.md 10 forbids all three for Phase 1).');
  L.push(`struct ${table.typeName} {`);
  for (const c of table.columns) {
    const comment = [];
    if (c.pk) comment.push('primary key');
    if (c.nullable) comment.push('nullable');
    if (c.maxLength > 0) comment.push(`max ${c.maxLength}`);
    const trailer = comment.length > 0 ? `  // ${comment.join(', ')}` : '';
    L.push(`    ${cppFieldType(c)} ${c.member}${memberInitializer(c)};${trailer}`);
  }
  L.push('');
  L.push(`    friend bool operator==(const ${table.typeName}&, const ${table.typeName}&) = default;`);
  L.push('};');
  L.push('');

  // ── width assertions ──
  const widthColumns = table.columns.filter(c => !c.nullable && c.fixedWidth !== null);
  if (widthColumns.length > 0) {
    L.push('// 🔴 Fixed width is asserted, not documented (cpp-style.md 4.1). Windows is LLP64 and Linux is');
    L.push('//    LP64, and this project moves between them on every deploy, so a field whose width drifts');
    L.push('//    must break the build here rather than corrupt a row in production.');
    for (const c of widthColumns) {
      L.push(`static_assert(sizeof(${table.typeName}::${c.member}) == ${c.fixedWidth}U,`);
      L.push(`              "${table.name}.${c.name} must stay ${c.fixedWidth} bytes wide");`);
    }
    L.push('');
  }

  // ── column metadata ──
  L.push('// Column metadata, in schema declaration order. The order is load bearing: the binding arrays');
  L.push('// below index into it.');
  L.push(`inline constexpr std::string_view ${p}Table = "${table.name}";`);
  L.push(`inline constexpr std::size_t ${p}ColumnCount = ${table.columns.length};`);
  L.push('');
  for (const c of table.columns) {
    L.push(`inline constexpr std::size_t ${p}Col${toPascalCase(c.name)} = ${c.index};`);
  }
  L.push('');
  L.push(`inline constexpr std::array<ColumnMeta, ${p}ColumnCount> ${p}Columns = {{`);
  for (const c of table.columns) {
    L.push(
      `    {.name_ = "${c.name}",` +
      ` .type_ = ColumnType::${c.columnType},` +
      ` .primary_key_ = ${c.pk},` +
      ` .nullable_ = ${c.nullable},` +
      ` .max_length_ = ${c.maxLength}},`);
  }
  L.push('}};');
  L.push('');
  const boundedColumns = table.columns.filter(c => c.maxLength > 0);
  if (boundedColumns.length > 0) {
    L.push('// Declared bound of each text column, so a length check does not have to repeat the number.');
    for (const c of boundedColumns) {
      L.push(`inline constexpr std::size_t ${p}${toPascalCase(c.name)}MaxLength = ${c.maxLength};`);
    }
    L.push('');
  }

  // ── prepared statements ──
  L.push('// ── Prepared statements ─────────────────────────────────────────────────────────────────────');
  L.push('// 🔴 Fixed text with `?` placeholders. Nothing here is concatenated, formatted or streamed at');
  L.push('//    run time (architecture-design.md 10), so no value can reach the statement text.');
  L.push('// Each statement is followed by the column indices its placeholders bind, in order, and by the');
  L.push('// static_assert that keeps the two in step.');
  L.push('');
  for (const s of statementsOf(table)) {
    const name = `${p}${s.suffix}`;
    for (const line of s.note) L.push(`// ${line}`);
    L.push(`inline constexpr std::string_view ${name}Sql =`);
    L.push(`    "${s.sql}";`);
    L.push(`inline constexpr std::array<std::size_t, ${s.binding.length}> ${name}Binding = {{`);
    for (const c of s.binding) L.push(`    ${p}Col${toPascalCase(c.name)},`);
    L.push('}};');
    L.push(`static_assert(CountPlaceholders(${name}Sql) == ${name}Binding.size(),`);
    L.push(`              "${table.name}: ${s.suffix} placeholders and binding order disagree");`);
    L.push('');
  }

  L.push(nsClose(), '');
  return L.join('\n');
}

function generateSourcesCMake(sourceFiles) {
  return [
    '# AUTO-GENERATED by tools/db_generator/db_generator.js — DO NOT EDIT.',
    '# 🔴 The source list is generated so that adding a table never means touching CMake, and so that',
    '#    a stale list cannot silently drop a row struct from the build. CMakeLists.txt includes this.',
    'set(ATLAS_GENERATED_DB_SOURCES',
    ...sourceFiles.map(f => `    ${f}`),
    ')',
    '',
  ].join('\n');
}

// ── Main ──────────────────────────────────────────────────────────────────────
function main() {
  const schema = loadSchema();
  const tables = analyze(schema);

  writeTextFile(SQL_OUTPUT, generateSql(schema, tables));

  const sourceFiles = ['db_meta.cpp'];
  writeTextFile(path.join(CPP_OUTPUT_DIR, 'db_meta.h'), generateMetaHeader());
  writeTextFile(
    path.join(CPP_OUTPUT_DIR, 'db_meta.cpp'),
    selfContainedSource('db_meta.h', 'The metadata vocabulary is header-only, so this translation unit exists for one reason: it proves the header is self-contained.'));
  for (const table of tables) {
    writeTextFile(path.join(CPP_OUTPUT_DIR, `${table.fileBase}.h`), generateRowHeader(table));
    writeTextFile(
      path.join(CPP_OUTPUT_DIR, `${table.fileBase}.cpp`),
      selfContainedSource(`${table.fileBase}.h`, `The \`${table.name}\` row header is all compile time constants, so this translation unit exists for one reason: it proves the header is self-contained.`));
    sourceFiles.push(`${table.fileBase}.cpp`);
  }
  writeTextFile(path.join(CPP_OUTPUT_DIR, 'db_sources.cmake'), generateSourcesCMake(sourceFiles));

  if (CHECK_ONLY && _state.changed.length > 0) {
    console.error(`[db] ERROR: generated DB artifacts are out of date (changed=${_state.changed.length}).`);
    for (const f of _state.changed) console.error(`[db]        ${f}`);
    console.error('[db]        Run `npm run gen:db` and commit the result.');
    process.exit(1);
  }

  const columnCount = tables.reduce((sum, t) => sum + t.columns.length, 0);
  console.log(`[db] tables=${tables.length} columns=${columnCount}`);
  console.log(`[db] output: ${toRel(CPP_OUTPUT_DIR)} (namespace ${CPP_NAMESPACE}) + ${toRel(SQL_OUTPUT)}`);
  console.log(`[db] files: changed=${_state.changed.length} unchanged=${_state.unchangedCount}`);
}

try {
  main();
} catch (e) {
  console.error('[db] Unexpected error:', e.message);
  if (e.stack) console.error(e.stack);
  process.exit(1);
}
