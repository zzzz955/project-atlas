'use strict';
/**
 * db_generator (C++ 타깃) — `server/db/schema.json` → MySQL `schema.sql` + C++ 행 구조체 / 컬럼
 * 메타데이터 / prepared statement 상수 / 바인딩 순서 배열.
 *
 * 스키마 한 벌에서 여러 타깃을 뽑는 것이 이 프레임워크의 seam 이다(architecture-design.md §14).
 * SQL 방출 로직은 자매 프로젝트(project-tower)의 db_generator 에서 그대로 가져왔고, 이 파일은
 * 거기에 **C++ emitter 를 추가**한 것이다. 경로 · 네임스페이스는 전부 `template.ini [db-gen]` 이
 * 소유한다 — 하드코딩된 게임/엔진 경로는 없다.
 *
 * 이 슬라이스의 경계 — **ORM 런타임을 만들지 않는다**(architecture-design.md §10).
 *    §10 의 ORM 은 "생성물 + 런타임" 두 부분이고, 런타임(커넥션 풀 · 트랜잭션 RAII ·
 *    per-character lock)은 아직 없다. 런타임이 없는데 CRUD 실행 코드를 뽑으면 컴파일되지 않는다.
 *    여기서 나오는 것: 행 구조체 · 컬럼 메타데이터 · placeholder SQL 상수 · 바인딩 순서 배열.
 *    여기서 나오지 않는 것: 실행 API · 커넥션 · 트랜잭션 · 락. 그것은 ORM 런타임 노드의 몫이고,
 *    그 노드가 생기면 이 emitter 를 확장해 CRUD 함수 시그니처까지 뽑는다.
 *
 * 라이브 DB 동기화는 **dev 전용**이다(architecture-design.md §10.1). 이 생성기는 스키마를
 *    적용하지 않고 **마이그레이션 SQL 파일을 낸다**(§10.7). `--apply` 만 그 파일을 실행하며, `.env` 의
 *    `ATLAS_ENV` 가 정확히 `dev` 가 아니면 접속 전에 거부하고 exit 1 한다. prod 마이그레이션은
 *    사람이 그 SQL 을 읽고 직접 적용한다 — 규약이 아니라 코드가 그렇게 만든다.
 *    접속 정보는 diff / apply 경로가 실제로 접속하는 순간에만 `server/.env` 에서 **lazy** 로
 *    읽고, `template.ini [db-gen]` 에는 접속·환경 키가 하나도 없다(§5.4).
 *
 * `--check` 는 DB 에 **접속하지 않는다.** `gen:check` 는 §15.4 게이트의 선두 단계이고 CI 에는
 *    MySQL 서비스가 없다 — 여기가 접속을 시도하는 순간 드리프트 게이트가 통째로 죽는다.
 *    같은 이유로 DB 미도달(또는 드라이버 미설치)은 **diff 단계만 skip** 하고 exit 0 이다.
 *
 * 문자열 SQL 조립 금지(architecture-design.md §10). 생성되는 SQL 은 `?` placeholder 가 박힌
 *    **고정 문자열 상수**이고, 컬럼명·값을 런타임에 이어 붙이는 코드는 뽑지 않는다.
 *
 * CLI:
 *   --check                    드리프트 검사만(파일을 쓰지 않고, DB 에 접속하지 않는다). 어긋나면 exit 1.
 *   --apply                    마이그레이션 SQL 을 dev DB 에 실행한다. ATLAS_ENV=dev 필수.
 *   --allow-drops              `--apply` 와 함께: MODIFY / DROP 까지 실행한다. 데이터 손실 경로다.
 *   --schema=<path>            테스트 전용 입력 경로 오버라이드.
 *   --cpp-output-dir=<path>    테스트 전용 C++ 출력 경로 오버라이드.
 *   --sql-output=<path>        테스트 전용 SQL 출력 경로 오버라이드.
 *   --migrations-dir=<path>    테스트 전용 마이그레이션 출력 경로 오버라이드.
 *   --env-file=<path>          테스트 전용 .env 경로 오버라이드.
 */

const fs = require('fs');
const path = require('path');
const cfg = require('../config-loader');
const { formatCxx } = require('../cxx-format');

const CHECK_ONLY = process.argv.includes('--check');
const APPLY = process.argv.includes('--apply');
const ALLOW_DROPS = process.argv.includes('--allow-drops');

function argValue(flag) {
  const hit = process.argv.find(a => a.startsWith(`${flag}=`));
  return hit ? hit.slice(flag.length + 1) : null;
}

const SCHEMA_PATH = path.resolve(argValue('--schema') ?? cfg.dbGen.schemaPath);
const CPP_OUTPUT_DIR = path.resolve(argValue('--cpp-output-dir') ?? cfg.dbGen.cppDbOutputDir);
const SQL_OUTPUT = path.resolve(argValue('--sql-output') ?? cfg.dbGen.sqlOutput);
const CPP_NAMESPACE = cfg.dbGen.cppNamespace;
// 마이그레이션 출력은 입력 스키마 옆(`server/db/migrations/`)이다. `template.ini` 에 키를 새로
// 만들지 않는다 — 이 노드의 규율은 "접속·환경 키를 ini 에 0개로 유지"이고, 스키마 경로 하나에서
// 파생시키면 ini 표면을 넓히지 않고도 경로가 여전히 ini 소유로 남는다.
const MIGRATIONS_DIR = path.resolve(
  argValue('--migrations-dir') ?? path.join(path.dirname(SCHEMA_PATH), 'migrations'));
// 접속 정보는 여기서 읽지 않는다 — 실제로 접속하는 순간에만 lazy 로 읽는다(dbEnv()).
const ENV_FILE = path.resolve(argValue('--env-file') ?? path.join(cfg.root, 'server', '.env'));
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

function fail(lines) {
  for (const line of lines) console.error(line);
  process.exit(1);
}

// ── 타입 해석 ────────────────────────────────────────────────────────────────────
// 정규화 타입 → C++ / MySQL 매핑의 SoT 는 `tools/types.json` 이다. 여기에 표를 복제하지 않는다.
// `string(N)` · `datetime` · `json` 만 표 밖의 특수 처리이며, 그 세 개는 아래 한 곳에서 다룬다.
const SPECIAL_TYPES = {
  datetime: { cppScalar: 'SysTime', columnType: 'DateTime', mysql: 'DATETIME', include: 'atlas/core/time.h' },
  json: { cppScalar: 'std::string', columnType: 'Json', mysql: 'JSON', include: null },
};

function parseStringN(type) {
  const m = /^string\((\d+)\)$/.exec(type);
  return m ? parseInt(m[1], 10) : null;
}

// 고정폭 바이트 수. 이 값은 생성 코드의 static_assert 로 그대로 박힌다 —
// 폭이 주석이 아니라 빌드 실패로 강제되는 지점이다(cpp-style.md §4.1).
const FIXED_WIDTHS = {
  int8: 1, uint8: 1, bool: 1,
  int16: 2, uint16: 2,
  int32: 4, uint32: 4, float: 4,
  int64: 8, uint64: 8, double: 8,
};

// 컬럼 이름 → 강타입 ID 매핑표(cpp-style.md §4.3, server/atlas/core/ids.h).
//    전부 `UInt64` 별칭으로 두면 `Load(server_id, character_id)` 에 인자 순서를 바꿔 넣어도
//    컴파일이 통과하고 런타임에 조용히 틀린다. `enum class` 는 암묵 변환이 없어 같은 실수가
//    컴파일 에러가 되며 런타임 비용은 0이다.
//    `server_id` 는 일부러 여기 없다 — `ids.h` 가 "인자로 돌아다니지 않는 값에 강타입을 주면
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
 * 검증을 전부 끝낸 뒤에 파일을 쓴다 — 뒤쪽 테이블이 거부당했는데 앞쪽 구조체만 새로 쓰인
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
    '-- Hand edits are silently lost on the next `npm run gen:db` and `npm run gen:db:check`',
    '--    fails the CI gate. Change server/db/schema.json instead.',
    '--',
    '-- This is the OFFLINE full-create script, not a migration. It is written without ever',
    '--    contacting a database, which is why it can be produced in CI where no MySQL exists.',
    '--    Incremental changes to a database that already has tables go to server/db/migrations/',
    '--    instead (architecture-design.md 10.7); on production a human applies those by hand.',
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
  '// =============================================================================',
  '// AUTO-GENERATED by tools/db_generator/db_generator.js - 직접 수정 금지',
  '// 손으로 고친 내용은 다음 gen:db 에서 사라지고 gen:db:check 가 CI 를 막음',
  '// server/db/schema.json 을 고칠 것',
  '// =============================================================================',
];

const SCOPE_NOTE = [
  '// [AD 10] 이 슬라이스가 내는 것은 데이터뿐이다',
  '// 행 구조체, 컬럼 메타데이터, prepared statement 문자열, 바인딩 순서',
  '// 실행은 없다. 커넥션/트랜잭션/per-character lock 은 ORM 런타임의 몫',
  '// 런타임 SQL 조립도 없다. 모든 문장은 `?` 가 박힌 고정 상수',
  '// 그래서 값이 문장 텍스트에 끼어들 수 없다',
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
    '',
    '// 생성 행 헤더가 공유하는 어휘',
    '// 컬럼의 저장 타입, 컴파일 타임 컬럼 서술자',
    '// 각 문장을 바인딩 순서에 묶는 플레이스홀더 카운터',
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
    '// [CS 4.3] schema.json 이 선언한 컬럼의 저장 타입',
    '// C++ 필드가 아니라 DB 컬럼을 서술한다',
    '// 식별자 컬럼은 UInt64 저장이지만 행 구조체는 강타입 ID 라 둘이 다르다',
    '// 열거자는 tools/types.json 정규화 타입 표에서 온다. 표에 없으면 못 쓴다',
    'enum class ColumnType : UInt8 {',
  ];
  columnTypeEnumerators().forEach((name, index) => L.push(`    ${name} = ${index},`));
  L.push(
    '};',
    '',
    '// 테이블 한 개의 컬럼 한 개. 일부러 aggregate',
    '// 모든 인스턴스는 런타임이 아니라 생성 헤더 안의 컴파일 타임 상수',
    'struct ColumnMeta {',
    '    std::string_view name_;',
    '    ColumnType type_{};',
    '    bool primary_key_{};',
    '    bool nullable_{};',
    '    std::size_t max_length_{};  // string(N) 의 N. 그 밖의 타입은 0',
    '',
    '    friend bool operator==(const ColumnMeta&, const ColumnMeta&) = default;',
    '};',
    '',
    '// prepared statement 안의 `?` 개수',
    '// "바인딩 배열이 문장과 맞는다"를 주석의 약속으로 두지 않기 위한 것',
    '// 생성 헤더마다 static_assert 로 굳힌다. 어긋나면 빌드가 깨진다',
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
    '',
    `// ${note}`,
    '// [AD 15.1] 그 증명을 실제 게이트로 만드는 것은 unity OFF 빌드',
    '// 여기서 빠진 include 는 이웃에 가려지지 않고 빌드를 깨뜨린다',
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
      note: ['기본 키로 한 행을 읽음'],
      sql: `SELECT ${all} FROM ${quoted(table.name)} WHERE ${pkPredicate}`,
      binding: table.pkColumns,
    },
    {
      suffix: 'Insert',
      note: ['한 행 전체를 삽입. 모든 컬럼을 선언 순서대로 바인딩'],
      sql: `INSERT INTO ${quoted(table.name)} (${all}) ` +
        `VALUES (${table.columns.map(() => '?').join(', ')})`,
      binding: table.columns,
    },
  ];
  if (table.nonPkColumns.length > 0) {
    statements.push({
      suffix: 'UpdateByPk',
      note: [
        '한 행의 비-키 컬럼 전부를 덮어씀',
        '키 컬럼은 SET 목록 뒤에 마지막으로 바인딩된다',
        '바인딩 배열을 컬럼 순서와 같다고 가정하지 않는 이유가 이 순서 차이',
      ],
      sql: `UPDATE ${quoted(table.name)} SET ` +
        `${table.nonPkColumns.map(c => `${quoted(c.name)} = ?`).join(', ')} WHERE ${pkPredicate}`,
      binding: [...table.nonPkColumns, ...table.pkColumns],
    });
  }
  statements.push({
    suffix: 'DeleteByPk',
    note: ['기본 키로 한 행을 삭제'],
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
  if (table.comment) L.push('', `// ${table.comment}`);
  L.push('', ...SCOPE_NOTE, '', '#pragma once', '');
  for (const i of includes.std) L.push(`#include ${i}`);
  L.push('');
  for (const i of includes.atlas) L.push(`#include "${i}"`);
  L.push(`#include "${INCLUDE_PREFIX}/db_meta.h"`);
  L.push('', nsOpen(), '');

  // ── row struct ──
  L.push('// [AD 10] 테이블의 한 행. 순수 aggregate');
  L.push('// 변경 추적도 지연 로딩도 unit of work 도 없다');
  L.push('// Phase 1 에서 셋 다 금지');
  L.push(`struct ${table.typeName} {`);
  for (const c of table.columns) {
    const comment = [];
    if (c.pk) comment.push('기본 키');
    if (c.nullable) comment.push('nullable');
    if (c.maxLength > 0) comment.push(`최대 ${c.maxLength}`);
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
    L.push('// [CS 4.1] 고정폭은 문서가 아니라 단언으로 강제한다');
    L.push('// Windows 는 LLP64, Linux 는 LP64 이고 배포마다 둘 사이를 오간다');
    L.push('// 폭이 밀리면 프로덕션 행을 망가뜨리기 전에 여기서 빌드가 깨져야 한다');
    for (const c of widthColumns) {
      L.push(`static_assert(sizeof(${table.typeName}::${c.member}) == ${c.fixedWidth}U,`);
      L.push(`              "${table.name}.${c.name} must stay ${c.fixedWidth} bytes wide");`);
    }
    L.push('');
  }

  // ── column metadata ──
  L.push('// 스키마 선언 순서대로의 컬럼 메타데이터');
  L.push('// 아래 바인딩 배열이 이 순서로 인덱싱하므로 순서 자체가 계약이다');
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
    L.push('// 각 텍스트 컬럼의 선언 상한. 길이 검사가 숫자를 다시 적지 않게 함');
    for (const c of boundedColumns) {
      L.push(`inline constexpr std::size_t ${p}${toPascalCase(c.name)}MaxLength = ${c.maxLength};`);
    }
    L.push('');
  }

  // ── prepared statements ──
  L.push('// =============================================================================');
  L.push('// Prepared statements');
  L.push('// =============================================================================');
  L.push('// [AD 10] `?` 플레이스홀더가 박힌 고정 텍스트');
  L.push('// 런타임에 잇거나 포맷하거나 스트리밍하지 않는다');
  L.push('// 그래서 어떤 값도 문장 텍스트에 닿지 못한다');
  L.push('// 문장마다 바인딩할 컬럼 인덱스와 static_assert 가 뒤따른다');
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

function generateAllHeader(tables) {
  return [
    ...BANNER,
    '',
    '// 집합 헤더이고 생성물이다',
    '// "행 구조체 전부가 include 하나로 들어온다"는 주장은 손으로 쓰면 썩는다',
    '// 테이블이 추가되는 순간 목록이 뒤처지기 때문',
    '',
    '#pragma once',
    '',
    `#include "${INCLUDE_PREFIX}/db_meta.h"`,
    ...tables.map(t => `#include "${INCLUDE_PREFIX}/${t.fileBase}.h"`),
    '',
  ].join('\n');
}

function generateSourcesCMake(sourceFiles) {
  return [
    '# AUTO-GENERATED by tools/db_generator/db_generator.js — DO NOT EDIT.',
    '# The source list is generated so that adding a table never means touching CMake, and so that',
    '#    a stale list cannot silently drop a row struct from the build. CMakeLists.txt includes this.',
    'set(ATLAS_GENERATED_DB_SOURCES',
    ...sourceFiles.map(f => `    ${f}`),
    ')',
    '',
  ].join('\n');
}

// ── 라이브 DB 동기화 (dev 전용) ───────────────────────────────────────────────
// 여기부터 아래는 `--check` 가 절대 밟지 않는 경로다. 위쪽 emitter 는 전부 오프라인이며,
// 그 오프라인성이 §15.4 드리프트 게이트의 전제다.

const MIGRATIONS_TABLE = 'schema_migrations';
// 파괴적 문장의 표식. 이 접두사가 붙은 줄은 **주석**이므로 파일을 그냥 실행하면 건너뛴다.
//    `--allow-drops` 만 접두사를 떼고 실행한다 — 데이터 손실 경로에 사람 판단을 강제하는 장치다.
const DESTRUCTIVE_PREFIX = '-- !destructive: ';
const ENV_KEYS = [
  'ATLAS_ENV', 'ATLAS_DB_HOST', 'ATLAS_DB_PORT', 'ATLAS_DB_NAME', 'ATLAS_DB_USER',
  'ATLAS_DB_PASSWORD',
];
const CONNECTION_KEYS = ['ATLAS_DB_HOST', 'ATLAS_DB_NAME', 'ATLAS_DB_USER', 'ATLAS_DB_PASSWORD'];

let _env = null;

/**
 * `.env` lazy 로드. 값은 절대 로그에 찍지 않는다 — 키 이름과 출처만 찍는다
 * (architecture-design.md §5.4). 프로세스 환경변수가 파일보다 우선한다(dotenv 규약):
 * 컨테이너/CI 가 파일 없이도 값을 주는 정상 경로이고, prod 게이트를 시험할 때도 그 경로를 쓴다.
 */
function dbEnv() {
  if (_env !== null) return _env;

  const fromFile = new Map();
  if (fs.existsSync(ENV_FILE)) {
    for (const raw of fs.readFileSync(ENV_FILE, 'utf-8').split(/\r?\n/)) {
      const line = raw.trim();
      if (!line || line.startsWith('#')) continue;
      const eq = line.indexOf('=');
      if (eq > 0) fromFile.set(line.slice(0, eq).trim(), line.slice(eq + 1).trim());
    }
  }

  const values = {};
  const sources = [];
  for (const key of ENV_KEYS) {
    const fromProcess = process.env[key];
    if (fromProcess !== undefined && fromProcess !== '') {
      values[key] = fromProcess;
      sources.push(`${key}=env`);
    } else if (fromFile.get(key)) {
      values[key] = fromFile.get(key);
      sources.push(`${key}=${toRel(ENV_FILE)}`);
    } else {
      sources.push(`${key}=unset`);
    }
  }
  console.log(`[db] env keys (names and source only, never values): ${sources.join(' ')}`);
  _env = values;
  return _env;
}

/**
 * prod 마이그레이션을 코드가 막는 지점. 문서 규약이 아니라 여기가 강제한다.
 * 미설정도 거부다 — 조용한 기본값을 두지 않는다(architecture-design.md §5.4).
 */
function requireDevEnvironment() {
  const declared = dbEnv().ATLAS_ENV;
  if (declared === 'dev') return;
  fail([
    '[db] ERROR: `npm run gen:db:apply` refused to touch the database.',
    `[db]        ATLAS_ENV is ${declared === undefined ? 'unset' : `"${declared}"`}; applying a`,
    '[db]        migration is a DEVELOPMENT-only path and needs exactly ATLAS_ENV=dev.',
    '[db]        Production migrations are applied by a human reading the SQL under',
    `[db]        ${toRel(MIGRATIONS_DIR)} — that is the whole point of emitting a file`,
    '[db]        (architecture-design.md 10.7). Nothing was sent to the database.',
  ]);
}

function connectionSettings() {
  const env = dbEnv();
  const missing = CONNECTION_KEYS.filter(k => env[k] === undefined);
  if (missing.length > 0) return { missing };
  return {
    missing: [],
    host: env.ATLAS_DB_HOST,
    port: Number.parseInt(env.ATLAS_DB_PORT ?? '3306', 10),
    database: env.ATLAS_DB_NAME,
    user: env.ATLAS_DB_USER,
    password: env.ATLAS_DB_PASSWORD,
  };
}

/**
 * `mysql2` 는 이 레포의 의존성이 아니다. CI 에는 MySQL 이 없어 이 경로가 아예 돌지 않고,
 * 게이트가 도는 데 필요하지도 않은 드라이버를 `npm ci` 가 매번 받아오게 만들 이유가 없다.
 * 필요할 때만 개발 머신에 깔아 쓴다: `npm install mysql2 --no-save`.
 */
function loadDriver() {
  try {
    return require('mysql2/promise');
  } catch {
    return null;
  }
}

/** SELECT 만 한다. `gen:db` 는 어떤 경로로도 DB 를 변경하지 않는다. */
async function readDbState(conn, database) {
  const [columns] = await conn.execute(
    'SELECT TABLE_NAME, COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE' +
    ' FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = ?' +
    ' ORDER BY TABLE_NAME, ORDINAL_POSITION', [database]);
  const [indexes] = await conn.execute(
    'SELECT TABLE_NAME, INDEX_NAME FROM INFORMATION_SCHEMA.STATISTICS' +
    ' WHERE TABLE_SCHEMA = ? GROUP BY TABLE_NAME, INDEX_NAME', [database]);

  const state = new Map();
  const slot = (name) => {
    if (!state.has(name)) state.set(name, { columns: new Map(), indexes: new Set() });
    return state.get(name);
  };
  for (const r of columns) {
    slot(r.TABLE_NAME).columns.set(
      r.COLUMN_NAME, { type: r.COLUMN_TYPE, nullable: r.IS_NULLABLE === 'YES' });
  }
  for (const r of indexes) slot(r.TABLE_NAME).indexes.add(r.INDEX_NAME);
  return state;
}

function normalizeSqlType(type) {
  return String(type).toLowerCase().replace(/\s+/g, ' ').trim();
}

function columnDefinitionSql(col) {
  return `${col.mysql}${col.nullable ? ' NULL' : ' NOT NULL'}` +
    `${col.auto ? ' AUTO_INCREMENT' : ''}${sqlDefault(col)}`;
}

/**
 * schema.json ↔ 라이브 DB diff. `{ body, deferred }` 를 돌려주고 아무것도 실행하지 않는다.
 * `schema_migrations` 는 diff 대상에서 제외한다 — 원장이 스스로를 마이그레이션 대상으로
 *    보면 매 실행마다 자기 자신을 DROP 하려 든다.
 */
function buildMigrationSql(tables, dbState) {
  const lines = [];
  const deferred = [];
  let executable = 0;
  const schemaTables = new Set(tables.map(t => t.name));

  for (const table of tables) {
    if (dbState.has(table.name)) continue;
    lines.push(generateTableSql(table), '');
    executable++;
  }

  for (const table of tables) {
    const live = dbState.get(table.name);
    if (live === undefined) continue;
    const adds = [];
    const destructive = [];

    for (const c of table.columns) {
      if (live.columns.has(c.name)) continue;
      adds.push(`  ADD COLUMN ${quoted(c.name)} ${columnDefinitionSql(c)}`);
      executable++;
    }

    for (const c of table.columns) {
      const liveCol = live.columns.get(c.name);
      if (liveCol === undefined) continue;
      const want = normalizeSqlType(c.mysql);
      if (want === normalizeSqlType(liveCol.type) && c.nullable === liveCol.nullable) continue;
      const before = `${normalizeSqlType(liveCol.type)} ${liveCol.nullable ? 'NULL' : 'NOT NULL'}`;
      const after = `${want} ${c.nullable ? 'NULL' : 'NOT NULL'}`;
      destructive.push({
        why: `${table.name}.${c.name}: ${before} -> ${after} (MODIFY can truncate stored values)`,
        stmt: `ALTER TABLE ${quoted(table.name)} MODIFY COLUMN ${quoted(c.name)} ` +
          `${columnDefinitionSql(c)};`,
      });
    }

    const schemaColumns = new Set(table.columns.map(c => c.name));
    for (const name of live.columns.keys()) {
      if (schemaColumns.has(name)) continue;
      destructive.push({
        why: `${table.name}.${name}: column is not in schema.json (DROP loses its data)`,
        stmt: `ALTER TABLE ${quoted(table.name)} DROP COLUMN ${quoted(name)};`,
      });
    }

    // 인덱스 기대 집합 = 컬럼 단위 unique(`uq_<table>_<column>`, generateTableSql 과 같은 이름) +
    // 스키마가 선언한 인덱스.
    const wanted = new Map();
    for (const c of table.columns) {
      if (c.unique) wanted.set(`uq_${table.name}_${c.name}`, { columns: [c.name], unique: true });
    }
    for (const idx of table.indexes) wanted.set(idx.name, idx);
    for (const [name, idx] of wanted) {
      if (live.indexes.has(name)) continue;
      adds.push(
        `  ADD ${idx.unique ? 'UNIQUE ' : ''}INDEX ${quoted(name)} ` +
        `(${idx.columns.map(quoted).join(', ')})`);
      executable++;
    }
    // FK 컬럼에는 InnoDB 가 제약 이름의 인덱스를 자동으로 만든다 — 스키마에 없다고 지우면 안 된다.
    const fkIndexes = new Set(table.columns.filter(c => c.fk).map(c => `fk_${table.name}_${c.name}`));
    for (const name of live.indexes) {
      if (name === 'PRIMARY' || wanted.has(name) || fkIndexes.has(name)) continue;
      destructive.push({
        why: `${table.name}: index "${name}" is not in schema.json`,
        stmt: `ALTER TABLE ${quoted(table.name)} DROP INDEX ${quoted(name)};`,
      });
    }

    if (adds.length > 0) lines.push(`ALTER TABLE ${quoted(table.name)}\n${adds.join(',\n')};`, '');
    for (const d of destructive) {
      lines.push(`-- ${d.why}`, `${DESTRUCTIVE_PREFIX}${d.stmt}`, '');
      deferred.push(d.why);
    }
  }

  for (const name of dbState.keys()) {
    if (name === MIGRATIONS_TABLE || schemaTables.has(name)) continue;
    const why = `${name}: table is not in schema.json (DROP loses every row in it)`;
    lines.push(`-- ${why}`, `${DESTRUCTIVE_PREFIX}DROP TABLE IF EXISTS ${quoted(name)};`, '');
    deferred.push(why);
  }

  if (executable === 0 && deferred.length === 0) return { body: null, deferred };
  return { body: lines.join('\n'), deferred };
}

function migrationHeader(stamp) {
  return [
    '-- AUTO-GENERATED by tools/db_generator/db_generator.js',
    `-- generated: ${stamp}`,
    `-- source: ${toRel(SCHEMA_PATH)}, diffed against a live database`,
    '--',
    '-- This file is the dev -> prod handover medium, so it is committed. `npm run gen:db:apply`',
    '--    executes it against a DEVELOPMENT database only (ATLAS_ENV=dev); on production a human',
    '--    reads it and applies it (architecture-design.md 10.7).',
    `-- Lines beginning with "${DESTRUCTIVE_PREFIX.trim()}" are destructive (MODIFY / DROP) and are`,
    '--    COMMENTS: running this script as-is skips them. Only `gen:db:apply -- --allow-drops`',
    '--    strips the marker and executes them.',
    '',
  ].join('\n');
}

function migrationFiles() {
  if (!fs.existsSync(MIGRATIONS_DIR)) return [];
  return fs.readdirSync(MIGRATIONS_DIR).filter(f => f.endsWith('.sql')).sort();
}

function withoutStamp(text) {
  return text.split('\n').filter(l => !l.startsWith('-- generated: ')).join('\n');
}

/**
 * 같은 diff 를 두 번 파일로 만들지 않는다. 그렇지 않으면 `gen:db` 를 세 번 돌린 사람이
 * 내용이 같은 마이그레이션 3개를 커밋하게 되고, 원장은 그 셋 중 어느 것이 진짜인지 말할 수 없다.
 */
function writeMigrationFile(body) {
  const stamp = new Date().toISOString();
  const content = `${migrationHeader(stamp)}${body}`;
  const existing = migrationFiles();
  if (existing.length > 0) {
    const newest = path.join(MIGRATIONS_DIR, existing[existing.length - 1]);
    if (withoutStamp(fs.readFileSync(newest, 'utf-8')) === withoutStamp(content)) {
      return { file: newest, reused: true };
    }
  }
  ensureDir(MIGRATIONS_DIR);
  const file = path.join(MIGRATIONS_DIR, `${stamp.replaceAll(/[:.]/g, '-').slice(0, 19)}_schema_sync.sql`);
  fs.writeFileSync(file, content, 'utf-8');
  return { file, reused: false };
}

/** 주석을 걷어내고 문장으로 쪼갠다. 파괴적 표식은 `--allow-drops` 일 때만 살아남는다. */
function executableStatements(content) {
  const kept = [];
  for (const line of content.split('\n')) {
    if (line.startsWith(DESTRUCTIVE_PREFIX)) {
      if (ALLOW_DROPS) kept.push(line.slice(DESTRUCTIVE_PREFIX.length));
      continue;
    }
    if (line.trimStart().startsWith('--')) continue;
    kept.push(line);
  }
  return kept.join('\n').split(';').map(s => s.trim()).filter(Boolean);
}

async function ensureLedger(conn) {
  await conn.query(
    `CREATE TABLE IF NOT EXISTS ${quoted(MIGRATIONS_TABLE)} (` +
    ` ${quoted('version')} VARCHAR(96) NOT NULL,` +
    ` ${quoted('applied_at')} DATETIME(6) NOT NULL,` +
    ` PRIMARY KEY (${quoted('version')})` +
    ') ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci');
}

async function recordMigration(conn, version) {
  await conn.execute(
    `INSERT INTO ${quoted(MIGRATIONS_TABLE)} (${quoted('version')}, ${quoted('applied_at')})` +
    ` VALUES (?, NOW(6)) ON DUPLICATE KEY UPDATE ${quoted('applied_at')} = NOW(6)`, [version]);
}

/** DB 에 닿지 못했을 때. `--apply` 는 실패이고, 그냥 `gen:db` 는 diff 단계만 건너뛴 성공이다. */
function unreachable(reason) {
  if (APPLY) {
    fail([
      `[db] ERROR: gen:db:apply cannot reach the database: ${reason}`,
      '[db]        Nothing was applied. `npm run gen:db` still emits schema.sql and the C++ output',
      '[db]        offline; only the live diff needs a database.',
    ]);
  }
  console.warn(`[db] live diff skipped — ${reason}`);
  console.warn('[db]        schema.sql and the C++ output are unaffected: they are produced offline.');
}

async function syncWithDatabase(tables) {
  const mysql2 = loadDriver();
  if (mysql2 === null) {
    unreachable('mysql2 is not installed (`npm install mysql2 --no-save`)');
    return;
  }
  const settings = connectionSettings();
  if (settings.missing.length > 0) {
    unreachable(`missing .env keys: ${settings.missing.join(', ')}`);
    return;
  }

  let conn;
  try {
    conn = await mysql2.createConnection({
      host: settings.host,
      port: settings.port,
      database: settings.database,
      user: settings.user,
      password: settings.password,
    });
  } catch (e) {
    unreachable(e.message);
    return;
  }

  try {
    const { body, deferred } = buildMigrationSql(tables, await readDbState(conn, settings.database));
    if (body === null) {
      console.log('[db] live database matches schema.json — no migration file.');
      return;
    }
    const { file, reused } = writeMigrationFile(body);
    console.log(`[db] migration ${reused ? 'unchanged' : 'written'}: ${toRel(file)}`);
    for (const d of deferred) console.log(`[db]        deferred (destructive): ${d}`);

    if (!APPLY) {
      console.log('[db] nothing was applied — `gen:db` only writes files. Use `npm run gen:db:apply`.');
      return;
    }

    const statements = executableStatements(fs.readFileSync(file, 'utf-8'));
    if (statements.length === 0) {
      console.log('[db] nothing executable in this migration; the ledger was not touched.');
      console.log('[db] Pass `-- --allow-drops` to execute the destructive statements above.');
      return;
    }
    await ensureLedger(conn);
    for (const stmt of statements) await conn.query(stmt);
    await recordMigration(conn, path.basename(file));
    console.log(
      `[db] applied statements=${statements.length} allow_drops=${ALLOW_DROPS}` +
      ` recorded in ${MIGRATIONS_TABLE}`);
    if (!ALLOW_DROPS && deferred.length > 0) {
      console.log(`[db] ${deferred.length} destructive change(s) were NOT applied (see above).`);
    }
  } finally {
    await conn.end();
  }
}

// ── Main ──────────────────────────────────────────────────────────────────────
async function main() {
  const schema = loadSchema();
  const tables = analyze(schema);

  writeTextFile(SQL_OUTPUT, generateSql(schema, tables));

  const sourceFiles = ['db_meta.cpp'];
  writeTextFile(path.join(CPP_OUTPUT_DIR, 'db_meta.h'), generateMetaHeader());
  writeTextFile(
    path.join(CPP_OUTPUT_DIR, 'db_meta.cpp'),
    selfContainedSource('db_meta.h', '메타데이터 어휘는 헤더 온리. 이 TU 는 헤더가 자기 완결임을 증명'));
  for (const table of tables) {
    writeTextFile(path.join(CPP_OUTPUT_DIR, `${table.fileBase}.h`), generateRowHeader(table));
    writeTextFile(
      path.join(CPP_OUTPUT_DIR, `${table.fileBase}.cpp`),
      selfContainedSource(`${table.fileBase}.h`, `\`${table.name}\` 행 헤더는 전부 컴파일 타임 상수. 이 TU 는 헤더가 자기 완결임을 증명`));
    sourceFiles.push(`${table.fileBase}.cpp`);
  }
  writeTextFile(path.join(CPP_OUTPUT_DIR, 'db_all.h'), generateAllHeader(tables));
  writeTextFile(path.join(CPP_OUTPUT_DIR, 'db_sources.cmake'), generateSourcesCMake(sourceFiles));

  if (CHECK_ONLY) {
    if (_state.changed.length > 0) {
      console.error(`[db] ERROR: generated DB artifacts are out of date (changed=${_state.changed.length}).`);
      for (const f of _state.changed) console.error(`[db]        ${f}`);
      console.error('[db]        Run `npm run gen:db` and commit the result.');
      process.exit(1);
    }
    // 여기서 반환한다. `--check` 는 DB 에 접속하지 않는다 — CI 에는 MySQL 이 없고, 이 단계는
    //    §15.4 게이트의 선두다. 접속을 시도하는 순간 드리프트 게이트 전체가 죽는다.
    console.log(`[db] artifacts up to date (unchanged=${_state.unchangedCount}).`);
    return;
  }

  const columnCount = tables.reduce((sum, t) => sum + t.columns.length, 0);
  console.log(`[db] tables=${tables.length} columns=${columnCount}`);
  console.log(`[db] output: ${toRel(CPP_OUTPUT_DIR)} (namespace ${CPP_NAMESPACE}) + ${toRel(SQL_OUTPUT)}`);
  console.log(`[db] files: changed=${_state.changed.length} unchanged=${_state.unchangedCount}`);

  // 환경 게이트가 접속보다 먼저다 — 거부는 소켓이 열리기 전에 일어나야 한다.
  if (APPLY) requireDevEnvironment();
  await syncWithDatabase(tables);
}

main().catch(e => {
  console.error('[db] Unexpected error:', e.message);
  if (e.stack) console.error(e.stack);
  process.exit(1);
});
