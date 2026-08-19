'use strict';
/**
 * pkt_generator (C++ 타깃) — `shared/contracts/**\/*.cs` (C# DTO/enum) → C++ 패킷 DTO.
 *
 * 계약 한 벌에서 여러 타깃을 뽑는 것이 이 프레임워크의 seam 이다(architecture-design.md §14).
 * C# 파서는 자매 프로젝트(project-tower)의 pkt_generator 에서 그대로 가져왔고, 이 파일은 거기에
 * **C++ emitter 를 추가**한 것이다. 경로 · 네임스페이스는 전부 `template.ini [packet-gen]` 이
 * 소유한다 — 하드코딩된 게임/엔진 경로는 없다.
 *
 * 지원: `public [sealed|partial] class { public T Prop { get; set|init; } }` + `public enum [: T]`.
 *
 * 이 생성기가 만들지 **않는** 것: 프레임 헤더 · opcode 테이블 · 시퀀스 · 체크섬 · HMAC.
 *    `design §8.1` 프레임 필드 폭과 `§16` HMAC/AEAD 가 아직 미결이라, 지금 헤더를 지어내면
 *    계약이 확정될 때 생성기와 생성물을 둘 다 재작업하게 된다. 여기서 만드는 것은 DTO 뿐이다.
 *
 * 확정 와이어 포맷 (design §8.5, cpp-style.md §4.4):
 *   - 엔디언       : 리틀엔디언 고정. 네트워크 바이트 오더(BE)를 따르지 않는다.
 *   - 길이 프리픽스 : UInt16 LE. 문자열(바이트 수) · 배열(원소 수) 공통. 상한 65535.
 *   - 문자열       : UTF-8, 종료 문자 없음, BOM 없음.
 *   - bool         : 1바이트, 0/1.
 *   - enum         : 계약에 선언된 고정 폭.
 *   - `#pragma pack` 없음 / 구조체 통짜 memcpy 없음 → 필드별 write/read 를 생성한다.
 *   - 부동소수점 필드는 거부한다(§8.4). 양자화된 정수만.
 *
 * CLI:
 *   --check                    드리프트 검사만(파일을 쓰지 않는다). 어긋나면 exit 1.
 *   --contracts-dir=<path>     테스트 전용 입력 경로 오버라이드.
 *   --cpp-output-dir=<path>    테스트 전용 출력 경로 오버라이드.
 */

const fs = require('fs');
const path = require('path');
const cfg = require('../config-loader');
const { formatCxx } = require('../cxx-format');

const CHECK_ONLY = process.argv.includes('--check');
const EXCLUDED_DIRS = new Set(['bin', 'obj', 'auth']);

// 길이 프리픽스 폭은 여기 한 곳에서만 정한다. 생성 코드 주석에도 같은 값이 박힌다.
const LENGTH_PREFIX_CPP = 'UInt16';
const LENGTH_PREFIX_MAX = 0xffff;

function argValue(flag) {
  const hit = process.argv.find(a => a.startsWith(`${flag}=`));
  return hit ? hit.slice(flag.length + 1) : null;
}

const CONTRACTS_DIR = path.resolve(argValue('--contracts-dir') ?? cfg.pktGen.contractsDir);
const CPP_OUTPUT_DIR = path.resolve(argValue('--cpp-output-dir') ?? cfg.pktGen.cppOutputDir);
const CPP_NAMESPACE = cfg.pktGen.cppNamespace;
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

function collectCSFiles(dir, out = []) {
  if (!fs.existsSync(dir)) return out;
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    if (entry.name.startsWith('_')) continue;
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      if (!EXCLUDED_DIRS.has(entry.name)) collectCSFiles(full, out);
    } else if (entry.name.endsWith('.cs')) {
      out.push(full);
    }
  }
  return out;
}

// ── 이름 변환 ────────────────────────────────────────────────────────────────────
function snake(name) {
  return name.replace(/([A-Z])/g, '_$1').toLowerCase().replace(/^_/, '');
}

// cpp-style.md §3 — 멤버는 후행 밑줄.
function member(name) {
  return `${snake(name)}_`;
}

// ── C# 파싱 ──────────────────────────────────────────────────────────────────────
function parseEnums(content) {
  const enums = [];
  // 원본 대비 확장: 기반 타입(`: byte`)을 캡처한다. 프로토콜에 실리므로 폭이 고정돼야 한다.
  const re = /public\s+enum\s+(\w+)\s*(?::\s*(\w+)\s*)?\{([\s\S]*?)\}/g;
  let m;
  while ((m = re.exec(content)) !== null) {
    const members = [];
    let next = 0;
    for (const entry of m[3].split(',').map(s => s.trim()).filter(Boolean)) {
      const mm = /^(\w+)\s*(?:=\s*(-?\d+))?$/.exec(entry);
      if (!mm) continue;
      // C# 의 암묵 값(직전 + 1)을 여기서 확정해 생성 코드에 명시한다 — 와이어 값은 눈에
      //    보여야 하고, 나중에 멤버를 끼워 넣었을 때 조용히 밀리면 안 된다.
      const value = mm[2] !== undefined ? parseInt(mm[2], 10) : next;
      next = value + 1;
      members.push({ name: mm[1], value });
    }
    enums.push({ name: m[1], baseCsType: m[2] ?? 'int', members });
  }
  return enums;
}

function parseClasses(content) {
  // 프로퍼티 accessor 제거 → 클래스 본문에 중괄호 없게 만들어 단순 매칭
  const flat = content.replace(/\{\s*get;\s*(?:set|init);\s*\}/g, ';');
  const classes = [];
  const re = /(?:public|internal)\s+(?:sealed\s+|partial\s+)*class\s+(\w+)\s*\{([\s\S]*?)\}/g;
  let m;
  while ((m = re.exec(flat)) !== null) {
    const props = [];
    const pre = /public\s+([\w<>?\[\],\s]+?)\s+(\w+)\s*(?:=\s*[^;]+)?;/g;
    let pm;
    while ((pm = pre.exec(m[2])) !== null) props.push({ type: pm[1].trim(), name: pm[2] });
    classes.push({ name: m[1], props });
  }
  return classes;
}

// ── 타입 해석 ────────────────────────────────────────────────────────────────────
// C# → 정규화 타입 → C++ 매핑의 SoT 는 `tools/types.json` 이다. 여기에 표를 복제하지 않는다.
const CSHARP_TO_NORMALIZED = new Map();
for (const [normalized, row] of Object.entries(cfg.types)) {
  if (row.csharp) CSHARP_TO_NORMALIZED.set(row.csharp, normalized);
}
const REJECTED_NORMALIZED = new Set(['float', 'double']);

function cppOf(normalized) {
  return cfg.types[normalized].cpp;
}

function whereOf(ctx) {
  return `${ctx.sourceRel}: ${ctx.owner}.${ctx.prop}`;
}

/** C# 타입 문자열 → 타입 노드. 지원하지 않는 타입은 여기서 즉사시킨다. */
function resolveType(csType, enumsByName, ctx, depth = 0) {
  const t = csType.replace(/\?$/, '').trim();

  const listMatch = /^(?:List|IList|IReadOnlyList|IEnumerable)<(.+)>$/.exec(t);
  const arrayMatch = /^(.+)\[\]$/.exec(t);
  const elementCs = listMatch ? listMatch[1] : arrayMatch ? arrayMatch[1] : null;
  if (elementCs !== null) {
    if (depth > 0) {
      fail([
        `[pkt] ERROR: ${whereOf(ctx)}: nested collection type "${csType}" is not supported.`,
        '[pkt]        Flatten it into parallel arrays (structure-of-arrays) instead —',
        '[pkt]        that is also what delta compression wants (architecture-design.md 8.4).',
      ]);
    }
    return { kind: 'array', elem: resolveType(elementCs, enumsByName, ctx, depth + 1) };
  }

  if (enumsByName.has(t)) {
    const e = enumsByName.get(t);
    return { kind: 'enum', name: t, cpp: e.cppBase };
  }

  const normalized = CSHARP_TO_NORMALIZED.get(t);
  if (normalized === undefined) {
    fail([
      `[pkt] ERROR: ${whereOf(ctx)}: C# type "${csType}" has no mapping.`,
      '[pkt]        Known types come from tools/types.json ("csharp" column) plus the enums',
      '[pkt]        declared in the contracts. Nested contract classes, DateTime and decimal are',
      '[pkt]        NOT supported by the C++ target — add the type to types.json (all 4 columns)',
      '[pkt]        or flatten the field.',
    ]);
  }
  if (REJECTED_NORMALIZED.has(normalized)) {
    fail([
      `[pkt] ERROR: ${whereOf(ctx)}: C# type "${csType}" is a floating point type.`,
      '[pkt]        Floating point is never carried on the wire (architecture-design.md 8.4,',
      '[pkt]        cpp-style.md 4.4): the representation differs between platforms and it costs',
      '[pkt]        bandwidth that quantization would have saved.',
      '[pkt]        Fix the contract: send a quantized integer instead (e.g. 1cm units as a short).',
    ]);
  }
  if (normalized === 'bool') return { kind: 'bool' };
  if (normalized === 'string') return { kind: 'string' };
  return { kind: 'scalar', cpp: cppOf(normalized) };
}

function cppTypeOf(node) {
  switch (node.kind) {
    case 'scalar': return node.cpp;
    case 'bool': return 'bool';
    case 'string': return 'std::string';
    case 'enum': return node.name;
    case 'array': return `std::vector<${cppTypeOf(node.elem)}>`;
    default: return 'void';
  }
}

// ── C++ 생성 ─────────────────────────────────────────────────────────────────────
const BANNER = [
  '// =============================================================================',
  '// AUTO-GENERATED by tools/pkt_generator/pkt_generator.js - 직접 수정 금지',
  '// 손으로 고친 내용은 다음 gen:pkt 에서 사라지고 gen:pkt:check 가 CI 를 막음',
  '// 계약 원본을 고칠 것',
  '// =============================================================================',
];

const WIRE_FORMAT_NOTE = [
  '// [AD 8.5] [CS 4.4] 와이어 포맷은 생성기가 고정한다',
  '// 리틀엔디언 고정. 문자열 UTF-8, bool 1바이트 0/1, enum 은 계약 폭',
  `// 가변 길이 앞에 ${LENGTH_PREFIX_CPP} LE 개수 접두. 상한 ${LENGTH_PREFIX_MAX}, 초과분은 잘림`,
  // 이 줄에 `#pragma` + `pack` 을 연달아 쓰지 않는다 — 게이트 grep 이 생성물 전체에서 그
  //    문자열을 0 으로 요구하고, 주석이 그 게이트를 오탐으로 죽인다.
  '// 패딩이 플랫폼마다 달라 통짜 복사 없이 필드 하나씩 쓰고 읽는다',
  '',
  '// [AD 8.4] 부동소수점 필드는 생성기가 거부한다. 양자화 정수만 실린다',
  '// [AD 11.2a] 읽기는 throw 하지 않는다. 깨진 버퍼는 Read 가 false',
];

function nsOpen() {
  return `namespace ${CPP_NAMESPACE} {`;
}

function nsClose() {
  return `}  // namespace ${CPP_NAMESPACE}`;
}

const UNSIGNED_WIDTHS = [
  { cpp: 'UInt8', bytes: 1 },
  { cpp: 'UInt16', bytes: 2 },
  { cpp: 'UInt32', bytes: 4 },
  { cpp: 'UInt64', bytes: 8 },
];
const SIGNED_WIDTHS = [
  { cpp: 'Int8', unsigned: 'UInt8' },
  { cpp: 'Int16', unsigned: 'UInt16' },
  { cpp: 'Int32', unsigned: 'UInt32' },
  { cpp: 'Int64', unsigned: 'UInt64' },
];

function generateCodecHeader() {
  const L = [
    ...BANNER,
    '',
    '// 생성 DTO 가 공유하는 리틀엔디언 코덱',
    '// 헤더 온리 inline 인 이유는 헬퍼가 명령 몇 개뿐이기 때문',
    '// 호출자도 생성 DTO 소스뿐이다',
    '',
    ...WIRE_FORMAT_NOTE,
    '',
    '#pragma once',
    '',
    '#include <cstddef>',
    '#include <span>',
    '#include <string>',
    '#include <vector>',
    '',
    '#include "atlas/core/types.h"',
    '',
    nsOpen(),
    '',
    `inline constexpr std::size_t kMaxPrefixedLength = ${LENGTH_PREFIX_MAX};  // ${LENGTH_PREFIX_CPP} 프리픽스 상한`,
    '',
  ];

  for (const w of UNSIGNED_WIDTHS) {
    L.push(`inline void WriteLe(std::vector<Byte>& out, ${w.cpp} value) {`);
    for (let i = 0; i < w.bytes; i++) {
      const shifted = i === 0 ? 'value' : `(value >> ${i * 8})`;
      L.push(`    out.push_back(static_cast<Byte>(${shifted} & 0xFFU));`);
    }
    L.push('}');
    L.push('');
  }
  for (const w of SIGNED_WIDTHS) {
    L.push(`inline void WriteLe(std::vector<Byte>& out, ${w.cpp} value) {`);
    L.push(`    WriteLe(out, static_cast<${w.unsigned}>(value));`);
    L.push('}');
    L.push('');
  }

  for (const w of UNSIGNED_WIDTHS) {
    L.push(`inline bool ReadLe(std::span<const Byte>& in, ${w.cpp}& value) {`);
    L.push('    // [CS 4.4] 크기 검사를 크기 산술보다 먼저 함. 잘린 버퍼에서 부호 없는');
    L.push('    // 뺄셈은 거대한 값으로 랩어라운드함');
    L.push(`    if (in.size() < ${w.bytes}U) return false;`);
    if (w.bytes === 1) {
      L.push(`    value = std::to_integer<${w.cpp}>(in[0]);`);
    } else {
      L.push(`    ${w.cpp} accumulated = 0;`);
      for (let i = 0; i < w.bytes; i++) {
        const shift = i === 0 ? '' : ` << ${i * 8}`;
        // `|=` 를 쓰지 않는다 — 정수 승격 때문에 UInt16 에서 int → UInt16 축소 경고(/W4)가 난다.
        L.push(`    accumulated = static_cast<${w.cpp}>(`);
        L.push(`        accumulated | static_cast<${w.cpp}>(std::to_integer<${w.cpp}>(in[${i}])${shift}));`);
      }
      L.push('    value = accumulated;');
    }
    L.push(`    in = in.subspan(${w.bytes}U);`);
    L.push('    return true;');
    L.push('}');
    L.push('');
  }
  for (const w of SIGNED_WIDTHS) {
    L.push(`inline bool ReadLe(std::span<const Byte>& in, ${w.cpp}& value) {`);
    L.push(`    ${w.unsigned} raw = 0;`);
    L.push('    if (!ReadLe(in, raw)) return false;');
    L.push('    // C++20 이 부호 있는 정수를 2의 보수로 고정하므로 이 변환은 정의된 동작');
    L.push(`    value = static_cast<${w.cpp}>(raw);`);
    L.push('    return true;');
    L.push('}');
    L.push('');
  }

  L.push(
    'inline void WriteBool(std::vector<Byte>& out, bool value) {',
    '    out.push_back(static_cast<Byte>(value ? 1U : 0U));',
    '}',
    '',
    'inline bool ReadBool(std::span<const Byte>& in, bool& value) {',
    '    UInt8 raw = 0;',
    '    if (!ReadLe(in, raw)) return false;',
    '    value = (raw != 0U);',
    '    return true;',
    '}',
    '',
    `// [AD 8.5] ${LENGTH_PREFIX_CPP} 길이 프리픽스를 쓰고 반영된 개수를 반환한다`,
    '// 상한 초과분은 스트림을 조용히 망가뜨리는 대신 잘라낸다',
    '// 프리픽스 폭이 고정이라 크기를 제한할 책임은 호출자에게 있다',
    `inline ${LENGTH_PREFIX_CPP} WriteLength(std::vector<Byte>& out, std::size_t count) {`,
    `    const ${LENGTH_PREFIX_CPP} clamped =`,
    `        static_cast<${LENGTH_PREFIX_CPP}>(count > kMaxPrefixedLength ? kMaxPrefixedLength : count);`,
    '    WriteLe(out, clamped);',
    '    return clamped;',
    '}',
    '',
    '// 길이 프리픽스 뒤의 UTF-8 페이로드. 프리픽스가 세는 것은 코드포인트가 아니라 바이트',
    'inline void WriteUtf8(std::vector<Byte>& out, const std::string& value) {',
    `    const ${LENGTH_PREFIX_CPP} length = WriteLength(out, value.size());`,
    '    for (std::size_t i = 0; i < static_cast<std::size_t>(length); ++i) {',
    '        out.push_back(static_cast<Byte>(static_cast<UInt8>(value[i])));',
    '    }',
    '}',
    '',
    'inline bool ReadUtf8(std::span<const Byte>& in, std::string& value) {',
    `    ${LENGTH_PREFIX_CPP} length = 0;`,
    '    if (!ReadLe(in, length)) return false;',
    '    // 선언된 길이를 할당 전에 남은 바이트와 대조한다',
    '    // 적대적인 프리픽스가 거대한 할당으로 이어지지 못하게 막는 지점',
    '    if (in.size() < static_cast<std::size_t>(length)) return false;',
    '    value.resize(static_cast<std::size_t>(length));',
    '    for (std::size_t i = 0; i < static_cast<std::size_t>(length); ++i) {',
    '        value[i] = static_cast<char>(std::to_integer<UInt8>(in[i]));',
    '    }',
    '    in = in.subspan(static_cast<std::size_t>(length));',
    '    return true;',
    '}',
    '',
    nsClose(),
    '',
  );
  return L.join('\n');
}

function generateCodecSource() {
  return [
    ...BANNER,
    '',
    '// 코덱은 헤더 온리라 이 TU 의 존재 이유는 하나뿐 - 헤더가 자기 완결임을 증명',
    '// [AD 15.1] 그 증명을 실제 게이트로 만드는 것은 unity OFF 빌드',
    '// 여기서 빠진 include 는 이웃에 가려지지 않고 빌드를 깨뜨린다',
    '',
    `#include "${INCLUDE_PREFIX}/pkt_codec.h"`,
    '',
  ].join('\n');
}

function generateEnumsHeader(enums) {
  const L = [
    ...BANNER,
    '',
    '// [CS 4.4] 계약 enum. 값이 와이어에 실리므로 기반 폭을 명시함',
    '// 멤버를 중간에 끼워 넣어도 뒤 값이 조용히 밀리지 않도록 열거자 값도 전부 명시',
    '',
    '#pragma once',
    '',
    '#include "atlas/core/types.h"',
    '',
    nsOpen(),
    '',
  ];
  for (const e of enums) {
    L.push(`// source: ${e.sourceRel}`);
    L.push(`enum class ${e.name} : ${e.cppBase} {`);
    for (const m of e.members) L.push(`    ${m.name} = ${m.value},`);
    L.push('};');
    L.push('');
  }
  L.push(nsClose(), '');
  return L.join('\n');
}

function emitWrite(node, expr, indent, depth, L) {
  const pad = ' '.repeat(indent);
  switch (node.kind) {
    case 'scalar':
      L.push(`${pad}WriteLe(out, ${expr});`);
      break;
    case 'bool':
      L.push(`${pad}WriteBool(out, ${expr});`);
      break;
    case 'string':
      L.push(`${pad}WriteUtf8(out, ${expr});`);
      break;
    case 'enum':
      L.push(`${pad}WriteLe(out, static_cast<${node.cpp}>(${expr}));`);
      break;
    case 'array': {
      const count = `count${depth}`;
      const index = `i${depth}`;
      L.push(`${pad}{`);
      L.push(`${pad}    const ${LENGTH_PREFIX_CPP} ${count} = WriteLength(out, ${expr}.size());`);
      L.push(`${pad}    for (std::size_t ${index} = 0; ${index} < static_cast<std::size_t>(${count}); ++${index}) {`);
      emitWrite(node.elem, `${expr}[${index}]`, indent + 8, depth + 1, L);
      L.push(`${pad}    }`);
      L.push(`${pad}}`);
      break;
    }
    default:
      break;
  }
}

function emitRead(node, target, indent, depth, L) {
  const pad = ' '.repeat(indent);
  switch (node.kind) {
    case 'scalar':
    case 'bool': {
      const fn = node.kind === 'bool' ? 'ReadBool' : 'ReadLe';
      L.push(`${pad}if (!${fn}(cursor, ${target})) return false;`);
      break;
    }
    case 'string':
      L.push(`${pad}if (!ReadUtf8(cursor, ${target})) return false;`);
      break;
    case 'enum': {
      const raw = `raw${depth}`;
      L.push(`${pad}{`);
      L.push(`${pad}    ${node.cpp} ${raw}{};`);
      L.push(`${pad}    if (!ReadLe(cursor, ${raw})) return false;`);
      L.push(`${pad}    // 모르는 열거자는 파싱 에러가 아님. 기반 타입이 고정이라 캐스팅은`);
      L.push(`${pad}    // 정의된 동작이고, 모르는 값을 거부할지는 호출자의 몫`);
      L.push(`${pad}    ${target} = static_cast<${node.name}>(${raw});`);
      L.push(`${pad}}`);
      break;
    }
    case 'array': {
      const count = `count${depth}`;
      const index = `i${depth}`;
      const element = `element${depth}`;
      L.push(`${pad}{`);
      L.push(`${pad}    ${LENGTH_PREFIX_CPP} ${count}{};`);
      L.push(`${pad}    if (!ReadLe(cursor, ${count})) return false;`);
      L.push(`${pad}    // 선언 개수로 reserve() 하지 않음 - 공격자가 정하는 값임. 원소를 하나씩`);
      L.push(`${pad}    // 덧붙여 거짓 프리픽스가 메모리 대신 버퍼를 먼저 소진하게 함`);
      L.push(`${pad}    for (std::size_t ${index} = 0; ${index} < static_cast<std::size_t>(${count}); ++${index}) {`);
      L.push(`${pad}        ${cppTypeOf(node.elem)} ${element}{};`);
      emitRead(node.elem, element, indent + 8, depth + 1, L);
      L.push(`${pad}        ${target}.push_back(${element});`);
      L.push(`${pad}    }`);
      L.push(`${pad}}`);
      break;
    }
    default:
      break;
  }
}

function usesKind(node, kind) {
  if (node.kind === kind) return true;
  return node.kind === 'array' && usesKind(node.elem, kind);
}

function generateClassHeader(cls, hasEnums) {
  const needsString = cls.fields.some(f => usesKind(f.type, 'string'));
  const needsVector = true; // Write() takes std::vector<Byte>&
  const needsEnums = hasEnums && cls.fields.some(f => usesKind(f.type, 'enum'));

  const includes = ['#include <span>'];
  if (needsString) includes.push('#include <string>');
  if (needsVector) includes.push('#include <vector>');

  const L = [
    ...BANNER,
    `// source: ${cls.sourceRel}`,
    '',
    ...WIRE_FORMAT_NOTE,
    '',
    '#pragma once',
    '',
    ...includes,
    '',
    '#include "atlas/core/types.h"',
  ];
  if (needsEnums) L.push(`#include "${INCLUDE_PREFIX}/contract_enums.h"`);
  L.push('', nsOpen(), '', `struct ${cls.name} {`);
  for (const f of cls.fields) L.push(`    ${cppTypeOf(f.type)} ${f.member}{};`);
  L.push('');
  L.push('    // 필드 하나씩 리틀엔디언으로 out 뒤에 덧붙임');
  L.push('    void Write(std::vector<Byte>& out) const;');
  L.push('');
  L.push('    // in 앞쪽에서 이 DTO 를 읽고 소비한 만큼 in 을 전진시킴');
  L.push('    // [AD 11.2a] 잘리거나 깨진 버퍼면 false 이고 throw 하지 않음');
  L.push('    // 실패 시 in 과 *this 는 건드리지 않음');
  L.push('    bool Read(std::span<const Byte>& in);');
  L.push('');
  L.push(`    friend bool operator==(const ${cls.name}&, const ${cls.name}&) = default;`);
  L.push('};');
  L.push('', nsClose(), '');
  return L.join('\n');
}

function generateClassSource(cls) {
  const L = [
    ...BANNER,
    `// source: ${cls.sourceRel}`,
    '',
    `#include "${INCLUDE_PREFIX}/${cls.fileBase}.h"`,
    '',
    '#include <cstddef>',
    '#include <utility>',
    '',
    `#include "${INCLUDE_PREFIX}/pkt_codec.h"`,
    '',
    nsOpen(),
    '',
    `void ${cls.name}::Write(std::vector<Byte>& out) const {`,
  ];
  for (const f of cls.fields) emitWrite(f.type, f.member, 4, 0, L);
  L.push('}', '');
  L.push(`bool ${cls.name}::Read(std::span<const Byte>& in) {`);
  L.push('    // 먼저 스크래치 복사본에 전부 디코드함. 중간에 실패해도 호출자가 반쯤');
  L.push('    // 덮어써진 DTO 를 들고 있지 않게 하려는 것');
  L.push('    std::span<const Byte> cursor = in;');
  L.push(`    ${cls.name} decoded;`);
  for (const f of cls.fields) emitRead(f.type, `decoded.${f.member}`, 4, 0, L);
  L.push('    *this = std::move(decoded);');
  L.push('    in = cursor;');
  L.push('    return true;');
  L.push('}');
  L.push('', nsClose(), '');
  return L.join('\n');
}

function generateSourcesCMake(sourceFiles) {
  return [
    '# AUTO-GENERATED by tools/pkt_generator/pkt_generator.js — DO NOT EDIT.',
    '# The source list is generated so that adding a contract never means touching CMake, and so',
    '#    that a stale list cannot silently drop a DTO from the build. CMakeLists.txt includes this.',
    'set(ATLAS_GENERATED_PKT_SOURCES',
    ...sourceFiles.map(f => `    ${f}`),
    ')',
    '',
  ].join('\n');
}

// ── Main ──────────────────────────────────────────────────────────────────────
function main() {
  const files = collectCSFiles(CONTRACTS_DIR).sort();
  if (files.length === 0) {
    fail([
      `[pkt] ERROR: no .cs contracts found in ${toRel(CONTRACTS_DIR)}.`,
      '[pkt]        pkt_generator has nothing to emit, and an empty output directory would silently',
      '[pkt]        drop every DTO from the build. Add a contract or fix [packet-gen].contracts_dir.',
    ]);
  }

  // 1-pass — enum/class 를 전부 수집한다. enum 이름이 필드 타입으로 쓰이므로 파싱을 먼저 끝낸다.
  const enums = [];
  const enumsByName = new Map();
  const rawClasses = [];
  for (const full of files) {
    const content = fs.readFileSync(full, 'utf-8');
    const sourceRel = toRel(full);
    for (const e of parseEnums(content)) {
      if (enumsByName.has(e.name)) {
        fail([`[pkt] ERROR: duplicate enum name "${e.name}" (${sourceRel}).`]);
      }
      const normalized = CSHARP_TO_NORMALIZED.get(e.baseCsType);
      if (normalized === undefined || REJECTED_NORMALIZED.has(normalized) || normalized === 'string' ||
          normalized === 'bool') {
        fail([
          `[pkt] ERROR: ${sourceRel}: enum "${e.name}" has underlying type "${e.baseCsType}".`,
          '[pkt]        The underlying type must be a fixed-width integer from tools/types.json',
          '[pkt]        because the value goes on the wire (cpp-style.md 4.4).',
        ]);
      }
      const record = { ...e, cppBase: cppOf(normalized), sourceRel };
      enums.push(record);
      enumsByName.set(e.name, record);
    }
    for (const cls of parseClasses(content)) rawClasses.push({ cls, sourceRel });
  }

  // 2-pass — 필드 타입 해석. 검증을 전부 끝낸 뒤에 파일을 쓴다: 뒤쪽 계약에서 거부당했는데
  // 앞쪽 DTO 는 이미 새로 쓰인 반쪽 출력이 남으면 안 된다.
  const classNames = new Set();
  const classes = [];
  for (const { cls, sourceRel } of rawClasses) {
    if (classNames.has(cls.name)) {
      fail([`[pkt] ERROR: duplicate class name "${cls.name}" (${sourceRel}).`]);
    }
    classNames.add(cls.name);
    const fields = cls.props.map(p => ({
      member: member(p.name),
      type: resolveType(p.type, enumsByName, { sourceRel, owner: cls.name, prop: p.name }),
    }));
    classes.push({ name: cls.name, fileBase: snake(cls.name), fields, sourceRel });
  }
  classes.sort((a, b) => a.fileBase.localeCompare(b.fileBase));

  // 3-pass — 방출.
  const sourceFiles = ['pkt_codec.cpp'];
  writeTextFile(path.join(CPP_OUTPUT_DIR, 'pkt_codec.h'), generateCodecHeader());
  writeTextFile(path.join(CPP_OUTPUT_DIR, 'pkt_codec.cpp'), generateCodecSource());
  if (enums.length > 0) {
    writeTextFile(path.join(CPP_OUTPUT_DIR, 'contract_enums.h'), generateEnumsHeader(enums));
  }
  for (const cls of classes) {
    writeTextFile(path.join(CPP_OUTPUT_DIR, `${cls.fileBase}.h`), generateClassHeader(cls, enums.length > 0));
    writeTextFile(path.join(CPP_OUTPUT_DIR, `${cls.fileBase}.cpp`), generateClassSource(cls));
    sourceFiles.push(`${cls.fileBase}.cpp`);
  }
  writeTextFile(path.join(CPP_OUTPUT_DIR, 'pkt_sources.cmake'), generateSourcesCMake(sourceFiles));

  if (CHECK_ONLY && _state.changed.length > 0) {
    console.error(`[pkt] ERROR: generated C++ contracts are out of date (changed=${_state.changed.length}).`);
    for (const f of _state.changed) console.error(`[pkt]        ${f}`);
    console.error('[pkt]        Run `npm run gen:pkt` and commit the result.');
    process.exit(1);
  }

  console.log(`[pkt] contracts=${files.length} classes=${classes.length} enums=${enums.length}`);
  console.log(`[pkt] output: ${toRel(CPP_OUTPUT_DIR)} (namespace ${CPP_NAMESPACE})`);
  console.log(`[pkt] files: changed=${_state.changed.length} unchanged=${_state.unchangedCount}`);
}

try {
  main();
} catch (e) {
  console.error('[pkt] Unexpected error:', e.message);
  if (e.stack) console.error(e.stack);
  process.exit(1);
}
