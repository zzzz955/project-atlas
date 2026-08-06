#!/usr/bin/env node
'use strict';

// project-atlas bulk-output guard (PreToolUse, matcher: Bash|PowerShell)
//
// Tool results are next-turn input tokens. This hook denies the shell commands
// that reliably blow the context budget and points at the bounded equivalent.
// It enforces the "Context budget" section of AGENTS.md mechanically instead of
// hoping the rule is remembered.
//
// Deliberately narrow — four rules, each with an explicit "bounded" escape so a
// legitimate bounded call passes untouched. False positives cost more than the
// tokens saved, so anything ambiguous is allowed through.
//
// Disable for debugging: ATLAS_BULK_OUTPUT_GUARD_DISABLE=1

const fs = require('fs');

if (process.env.ATLAS_BULK_OUTPUT_GUARD_DISABLE === '1') process.exit(0);

function readInput() {
  try {
    const raw = fs.readFileSync(0, 'utf8').trim();
    return raw ? JSON.parse(raw) : {};
  } catch (_) {
    return {};
  }
}

// A bound is anything that caps how much comes back.
const BOUNDED =
  /(-Directory|-File\s+-Name|-TotalCount|-Tail|-Depth|-First|-maxdepth|-Newest|\bhead\b|\btail\b|\bwc\b|--oneline|--stat|--name-only|--name-status|--format|--pretty|-n\s*\d|\s-\d+\b|-m\s*\d|Measure-Object)/i;

// Leading command = start of line, or after ; && || (NOT after a pipe, so
// `something | grep foo` stays legal as a filter).
const lead = (names) =>
  new RegExp(String.raw`(^|[;&]{1,2}|\n)\s*(${names})\b`, 'i');

const RULES = [
  {
    name: 'recursive-listing',
    test: (c) =>
      /(-Recurse\b|\bfind\s+[^|]*-type\b|\bls\s+-\w*R|\btree\b)/i.test(c),
    reason:
      'Unbounded recursive listing. Bound it at the source: add -Directory, a -Depth/-maxdepth ' +
      'predicate, or `| Select-Object -First N`. For "which files match a pattern", use the Glob ' +
      'tool instead — it returns paths only.',
  },
  {
    name: 'whole-file-dump',
    test: (c) => lead('cat|type|more|less|Get-Content|gc').test(c),
    reason:
      'Whole-file dump through the shell. Use the Read tool (it paginates and the harness tracks ' +
      'what is already in context). If you truly need the shell, bound it: -TotalCount / -Tail / ' +
      'head / tail.',
  },
  {
    name: 'shell-content-search',
    test: (c) => lead('grep|rg|egrep|fgrep|findstr|Select-String|sls').test(c),
    reason:
      'Content search through the shell. Use the Grep tool with head_limit / ' +
      'output_mode:"files_with_matches" — results are bounded and render as file links. ' +
      '(Using grep as a PIPE filter after another command is fine and not blocked.)',
  },
  {
    name: 'unbounded-git-log',
    test: (c) => /\bgit\b[^|;&]*\blog\b/i.test(c),
    reason:
      'Unbounded `git log`. Add -1 / -n N / --oneline / --format=… / --stat.',
  },
];

const input = readInput();
const command = String((input.tool_input && input.tool_input.command) || '');
if (!command) process.exit(0);

// `git diff` for commit work needs the full hunks — exempt the whole command.
if (/\bgit\b[^|;&]*\bdiff\b/i.test(command)) process.exit(0);

if (BOUNDED.test(command)) process.exit(0);

const hit = RULES.find((r) => r.test(command));
if (!hit) process.exit(0);

process.stdout.write(
  JSON.stringify({
    hookSpecificOutput: {
      hookEventName: 'PreToolUse',
      permissionDecision: 'deny',
      permissionDecisionReason:
        `[bulk-output-guard: ${hit.name}] ${hit.reason}\n` +
        'See the "Context budget" section of AGENTS.md. ' +
        'For broad exploration, prefer a read-only subagent — its raw output stays out of this context.',
    },
  }),
);
