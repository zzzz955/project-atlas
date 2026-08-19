'use strict';
/**
 * cxx-format — 생성된 C++ 소스를 저장소 `.clang-format` 으로 통과시키는 공유 헬퍼.
 *
 * 생성기 3종(`info` · `db` · `pkt`)은 모두 `writeTextFile(filePath, content)` 라는 같은 초크포인트를
 * 가지므로, 그 한 곳에서 포맷을 걸면 emit 문자열의 중괄호·공백 스타일을 손으로 맞출 필요가 없다.
 * `.clang-format` 이 바뀌면 생성 코드가 자동으로 따라온다(cpp-style.md §7.1).
 *
 * `--check` 경로와 쓰기 경로가 **같은 content** 를 보게 해야 한다. 포맷을 쓰기 직전이 아니라
 *    `writeTextFile` 진입부에서 걸어야 `gen:check` 가 드리프트를 오탐하지 않는다.
 *
 * clang-format 은 **19 이상**을 요구한다.
 *    - `SpacesInParens` 는 17+ 문법이라 그 미만에서는 `.clang-format` 파싱이 통째로 실패한다.
 *    - 18.1.3 은 `ColumnLimit` 을 열이 아니라 바이트로 세어 한글 주석의 줄바꿈 위치가 달라진다
 *      (architecture-design.md §15.5f).
 *    두 경우 모두 조용히 다른 결과를 내므로, 버전 미달은 경고가 아니라 실패로 처리한다.
 *
 * 탐색 순서: `ATLAS_CLANG_FORMAT` → `clang-format-19` → VS 2022 동봉 → PATH 의 `clang-format`.
 * PATH 의 `clang-format` 을 마지막에 두는 이유: ubuntu-24.04 러너에서 그 이름은 18 이다
 *    (architecture-design.md §15.5g).
 */

const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const MIN_MAJOR = 19;

function versionOf(exe) {
  try {
    const out = execFileSync(exe, ['--version'], { encoding: 'utf-8', stdio: ['ignore', 'pipe', 'ignore'] });
    const hit = /version\s+(\d+)\./.exec(out);
    return hit ? Number(hit[1]) : null;
  } catch {
    return null;
  }
}

function vsBundled() {
  const roots = [process.env['ProgramFiles'], process.env['ProgramFiles(x86)']].filter(Boolean);
  const found = [];
  for (const root of roots) {
    const vs = path.join(root, 'Microsoft Visual Studio', '2022');
    if (!fs.existsSync(vs)) continue;
    for (const edition of fs.readdirSync(vs)) {
      const exe = path.join(vs, edition, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang-format.exe');
      if (fs.existsSync(exe)) found.push(exe);
    }
  }
  return found;
}

let _resolved;

function resolveClangFormat() {
  if (_resolved) return _resolved;

  const candidates = [
    process.env.ATLAS_CLANG_FORMAT,
    'clang-format-19',
    ...vsBundled(),
    'clang-format',
  ].filter(Boolean);

  const rejected = [];
  for (const exe of candidates) {
    const major = versionOf(exe);
    if (major === null) continue;
    if (major < MIN_MAJOR) {
      rejected.push(`${exe} (버전 ${major})`);
      continue;
    }
    _resolved = exe;
    return _resolved;
  }

  const detail = rejected.length ? `\n  버전 미달로 거부: ${rejected.join(', ')}` : '';
  throw new Error(
    `clang-format ${MIN_MAJOR} 이상을 찾지 못했다. 생성 코드를 포맷할 수 없다.${detail}\n` +
      '  해결: VS 2022 를 설치하거나, ATLAS_CLANG_FORMAT 에 실행 파일 경로를 지정한다.'
  );
}

/**
 * `.h` / `.cpp` 만 포맷한다. 그 외 확장자는 그대로 돌려준다.
 * `--assume-filename` 을 넘겨야 `--style=file` 이 그 경로에서 위로 올라가며 `.clang-format` 을 찾는다.
 */
function formatCxx(filePath, content) {
  if (!/\.(h|hpp|cpp)$/i.test(filePath)) return content;

  const formatted = execFileSync(
    resolveClangFormat(),
    ['--style=file', `--assume-filename=${path.resolve(filePath)}`],
    { input: content, encoding: 'utf-8', maxBuffer: 64 * 1024 * 1024 }
  );

  // .gitattributes 가 트리 전체를 eol=lf 로 고정한다. 파이프가 CRLF 를 흘리면 되돌린다.
  return formatted.replaceAll('\r\n', '\n');
}

module.exports = { formatCxx };
