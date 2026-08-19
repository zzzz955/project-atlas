'use strict';
/**
 * project-atlas 코어 순수성 검사기 (`architecture-design.md §15.4`).
 *
 * "코어는 게임을 알면 안 된다"(§14 코어/계약/게임 3층)의 기계 강제다.
 * `gen:check` 가 "생성물 손편집"에 대해 하는 일을, 이 검사기가 "코어 오염"에 대해 한다.
 *
 * 생성기가 아니라 **검사기**다 — 아무것도 쓰지 않고 exit code 로만 말한다.
 * 로컬 게이트(PowerShell)와 CI(bash)가 같은 명령(`npm run check:core-purity`)을 부르도록
 *    Node 한 벌로 구현한다. 스크립트를 두 벌 쓰면 두 게이트가 조용히 갈라진다.
 *
 * 검사 2가지:
 *   규칙 1 (구조) — `server/atlas/**` 는 `generated/` 아래 헤더를 include 하지 않는다.
 *                   코어는 opcode 로 디스패치하지 특정 게임 DTO 를 알지 않는다.
 *                   예외는 `allowlist.txt` 로만 만든다(오늘 항목 0개).
 *   규칙 2 (어휘) — `server/atlas/**` 에 `denylist.txt` 의 용어가 0건.
 *
 * 사용:
 *   node tools/core_purity/core_purity.js
 *   node tools/core_purity/core_purity.js --root <dir>   # 픽스처 검사용 루트 교체
 */
const fs = require('fs');
const path = require('path');

const TOOL_DIR = __dirname;
const REPO_ROOT = path.resolve(TOOL_DIR, '..', '..');
const DEFAULT_ROOT = path.join(REPO_ROOT, 'server', 'atlas');
const ALLOWLIST = path.join(TOOL_DIR, 'allowlist.txt');
const DENYLIST = path.join(TOOL_DIR, 'denylist.txt');

// 검사 대상에서 제외: 생성물(손으로 쓰지 않는다)과 테스트(게임 어휘를 써도 된다).
// 주석/코드는 구분하지 않는다 — 주석에 게임 이름이 박히는 것도 오염이다.
const SKIP_DIRS = new Set(['generated', 'tests']);
const SOURCE_EXT = new Set(['.h', '.cpp']);

function fail(message) {
  console.error(`[core-purity] ERROR: ${message}`);
  process.exit(1);
}

function parseArgs(argv) {
  let root = DEFAULT_ROOT;
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--root') {
      if (!argv[i + 1]) fail('--root 뒤에 디렉터리가 없다');
      root = path.resolve(process.cwd(), argv[++i]);
    } else {
      fail(`알 수 없는 인자 "${argv[i]}"`);
    }
  }
  return root;
}

function readListFile(file) {
  if (!fs.existsSync(file)) fail(`목록 파일이 없다: ${rel(file)}`);
  return fs.readFileSync(file, 'utf8').split(/\r?\n/);
}

function rel(p) {
  return path.relative(REPO_ROOT, p).replaceAll(path.sep, '/');
}

/**
 * 허용 목록. 각 줄은 `<include 경로>  # <사유> (§절번호)` 형식이다.
 * 사유 없는 줄은 거부한다 — 사유 없이 늘어나는 예외는 검사 자체를 무의미하게 만든다.
 */
function loadAllowlist() {
  const allowed = new Set();
  readListFile(ALLOWLIST).forEach((raw, index) => {
    const line = raw.trim();
    if (!line || line.startsWith('#')) return;
    const hash = line.indexOf('#');
    if (hash < 0) {
      fail(`${rel(ALLOWLIST)}:${index + 1}: 사유가 없다 — "<경로>  # <사유> (§절번호)" 형식이어야 한다`);
    }
    const includePath = line.slice(0, hash).trim();
    const reason = line.slice(hash + 1).trim();
    if (!includePath) fail(`${rel(ALLOWLIST)}:${index + 1}: 경로가 비었다`);
    if (!reason) {
      fail(`${rel(ALLOWLIST)}:${index + 1}: 사유가 비었다 — "<경로>  # <사유> (§절번호)" 형식이어야 한다`);
    }
    allowed.add(includePath);
  });
  return allowed;
}

function loadDenylist() {
  const terms = [];
  readListFile(DENYLIST).forEach((raw) => {
    const line = raw.trim();
    if (!line || line.startsWith('#')) return;
    terms.push(line);
  });
  if (terms.length === 0) fail(`${rel(DENYLIST)}: 용어가 하나도 없다`);
  // 대소문자 무시 **부분 문자열** 매칭이다. 단어 경계로 맞추면 `boss_id` · `ApplySkillDamage`
  //    처럼 게임 어휘가 실제로 코드에 나타나는 형태를 전부 놓친다(측정: 두 형태 모두 \b 로는 0건).
  //    대가는 거짓 양성이며(`mob` 이 `mobility` 를 잡는다), 그때는 목록에서 그 용어를 빼는 것이
  //    답이지 매칭을 느슨하게 하는 것이 아니다.
  return terms.map((term) => ({ term, needle: term.toLowerCase() }));
}

function collectSources(root) {
  if (!fs.existsSync(root)) fail(`스캔 루트가 없다: ${root}`);
  const files = [];
  const walk = (dir) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) {
        if (SKIP_DIRS.has(entry.name)) continue;
        walk(full);
      } else if (SOURCE_EXT.has(path.extname(entry.name))) {
        files.push(full);
      }
    }
  };
  walk(root);
  return files;
}

const INCLUDE_RE = /^\s*#\s*include\s*[<"]([^>"]+)[>"]/;

function scanFile(file, allowed, deny) {
  const violations = [];
  const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/);
  lines.forEach((line, index) => {
    const where = `${rel(file)}:${index + 1}`;

    const inc = line.match(INCLUDE_RE);
    if (inc && inc[1].includes('generated/') && !allowed.has(inc[1])) {
      violations.push(`${where}: rule1(core includes generated contract) - ${line.trim()}`);
    }

    const lower = line.toLowerCase();
    for (const { term, needle } of deny) {
      if (lower.includes(needle)) {
        violations.push(`${where}: rule2(game vocabulary "${term}") - ${line.trim()}`);
      }
    }
  });
  return violations;
}

function main() {
  const root = parseArgs(process.argv.slice(2));
  const allowed = loadAllowlist();
  const deny = loadDenylist();
  const files = collectSources(root);

  // 0개 파일 통과가 이 검사의 최악 실패 모드다 — 경로 오타 하나로 게이트가 영원히 초록불이 된다.
  if (files.length === 0) {
    console.error(`[core-purity] ERROR: 스캔한 파일이 0개다 (root: ${root}). 경로를 확인하라.`);
    process.exit(1);
  }

  const violations = files.flatMap((file) => scanFile(file, allowed, deny));
  if (violations.length > 0) {
    for (const v of violations) console.error(`[core-purity] ${v}`);
    console.error(`[core-purity] FAILED: ${violations.length} violation(s) in ${files.length} file(s) scanned`);
    process.exit(1);
  }

  console.log(`[core-purity] clean (${files.length} files scanned)`);
}

main();
