# project-atlas

**C++ MMO game-server framework.** Not a game — a reusable server core that any client can be
swapped onto. The demo game exists only to exercise the core.

**Primary purpose = job-search artifact**: prove C++ / async IO / TCP / serialization /
multithreading / load at scale. 🔴 **Revenue is explicitly not a goal of this project.**

Status: **core layers landing** (2026-08-10). Built: `atlas/config`, `atlas/core` (ctx/log/error/ids/
types), `atlas/net` (acceptor/session/io_runner — byte boundary), `generated/{pkt,db}`. Not built:
frame layer, DB runtime, any server binary.

> Doc language: `AGENTS.md` is English and token-efficient (AI context). Human design docs under
> `docs/` are Korean. Chat replies follow the user's language preference.

## Nav
| path | role | doc |
|------|------|-----|
| `docs/design/architecture-design.md` | **Top-level SoT.** Purpose, Phase-1 success criteria, topology (FE/GAME/WORLD), config sources `§5.4`, 3-tier identity, Actor/AoI/BT, protocol + integrity layers, thread model + ctx, custom ORM + Redis policy `§10.2`, logging/exception policy, templatization seam (4 layers, incl. deploy/stack config), build chain — compose deploy `§15.3`, CI gate `§15.4`, CI infra/vcpkg cache `§15.5` — open items, risks | — |
| `docs/conventions/cpp-style.md` | **Coding convention SoT.** Namespace, naming, type rules (fixed-width ints, A-plan aliases, strong-typed IDs), macro policy, template policy, and the 3-layer mechanical enforcement | — |

## Domain matrix (trigger → required reading, BEFORE planning or editing)
Re-classify **every** user message against this table. A section loaded in an earlier turn does not
exempt this turn. Read the listed sections into context before you plan; do **not** re-read what is
already fully in context. `AD` = `docs/design/architecture-design.md`, `CS` = `docs/conventions/cpp-style.md`.

| Trigger phrases | Domain | Required reading |
|---|---|---|
| packet · frame · serialization · DTO · checksum · HMAC · sequence number · bandwidth · delta/quantization | Protocol | `AD §8` (+ `CS §4.1` int/long ban) |
| thread · strand · io_context · concurrency · lock · mutex · semaphore · ctx | Thread model | `AD §9`, `§9.1`, `§9.2` |
| exception · async handler · `Guarded` · logging · log macro · crash | Logging / exception | `AD §11` (+ `CS §5` macro policy) |
| ORM · DB · `schema.json` · persistence · transaction · prepared statement · per-character lock | Persistence | `AD §10` (+ `CS §4.3` strong-typed IDs) |
| Actor · AoI · behaviour tree · tick · room · combat · coordinates | WORLD model | `AD §7` |
| topology · FE · GAME · WORLD · routing · registry · heartbeat · InterWorld · Relay · attach/detach | Topology | `AD §5`, `§5.1`, `§5.2` |
| identity · account · character · session id · 3-tier | Identity | `AD §6` |
| auth · JWT · JWKS · `platform-auth` · login | Auth integration | `AD §12` |
| generator · `pkt_generator` · `db_generator` · `info_generator` · CSV · contracts · templatization · game swap | Generator seam | `AD §14` |
| CMake · vcpkg · PCH · unity build · Docker · CI gate · clang-tidy · clang-format | Build chain | `AD §15` + `CS §7` |
| compose · docker · `.env` · `server.ini` · role 설정 · Redis · 배포 | Infra | `AD §5.4`, `§10.2`, `§15.3`, `§15.5` |
| namespace · naming · type alias · macro · template · header hygiene | Code style | `CS §2`–`§6` |
| scope · phase · MVP · demo game content · what to build next | Scope | `AD §2`, `§3` (esp. `§3.3`) |
| server cheat · packet log · ops tool | Ops tooling | `AD §13` |
| decompose the work · DAG · parallel execution · wp files | Work orchestration | `Skill: plan-work` / `Skill: run-work` |

**Routing notes**
- Multiple rows can fire — load all of them.
- Any row that touches **protocol / auth / cross-server contracts** also fires the Clarification
  Protocol below: ask **before** implementing.
- Read the matched section, not the whole doc; but never read a section with `head`/`tail`-style
  truncation that cuts it mid-rule.

## Servers (planned)
| server | lifetime | role |
|--------|----------|------|
| **FE** | resident | connection termination · packet routing · WORLD registry · heartbeat |
| **GAME** | **fixed** | persistence · character · inventory · growth · mail · owns the ORM |
| **WORLD** | **dynamic attach/detach** | coordinates · combat · events · rooms · Actor · AoI · BT · tick loop |
| Relay / Matching / InterWorld | Phase 3 | cross-server content. **Not built in MVP, but their seam is fixed now** |

Lobby is **not a separate server**: auth = `platform-auth` over HTTP/JWKS (existing C# asset),
server/character selection = GAME.

## ⚖️ Mandates (zero tolerance)
- **SoT:** `docs/design/architecture-design.md` for architecture, `docs/conventions/cpp-style.md` for
  code style. If a doc contradicts the code, the code is truth — fix the doc.
- 🔴 **WORLD ↔ WORLD direct communication is forbidden.** All inter-server traffic goes through FE
  (or, in Phase 3, Relay). Breaking this now turns Phase 3 into a full redesign.
- 🔴 **WORLD and InterWorld are the same binary**, distinguished by ini config (`role`, `world_id`,
  `server_group`). InterWorld is a configuration variant, not a new server.
- 🔴 **Redis pub/sub is never used for inter-server game traffic** — it is the bypass route around the
  WORLD ↔ WORLD rule above, and taking it deletes the Phase-3 Relay seam. `AD §10.2`.
- 🔴 **Secrets live in `.env` only, never in `server.ini`.** `server.ini` is committed and holds
  role/ids/ports/workers/log config; credentials and hosts stay in `.env` (`*.example` only). The
  config loader logs secret **key names, never values**. `AD §5.4`.
- 🔴 **`int` / `long` are banned** in protocol / persistence / ID layers — Windows is LLP64, Linux is
  LP64, so `long` differs in size between dev and prod. Fixed-width aliases only (`Int32`, `UInt64`).
- 🔴 **Never `#pragma pack` a packet struct.** `pkt_generator` emits per-field write/read.
- 🔴 **Checksum is not tamper protection** — it guards framing integrity only. Tampering is stopped by
  session-key HMAC, and the final line is server authority.
- 🔴 **Exceptions must never escape an async handler** — one escaped exception kills an I/O thread and
  silently halts every session it served. All handler entry points go through `Guarded`.
- 🔴 **Concurrency is strands, single model.** Locks only on genuinely shared resources. A semaphore is
  not a mutex substitute (no ownership, generally slower); its valid use is handoff signalling / pool
  sizing.
- 🔴 **Never edit generated output.** Change the source (CSV / `schema.json` / contracts) and re-run
  the generator.
- 🔴 **The demo game's minimum set (design doc §3.3) is never expanded** without first answering which
  untested core path the addition exercises.
- 🔴 **Git is user-driven.** An agent never creates worktrees or branches, and never commits, on its
  own initiative — only on an explicit request. **No commit-message trailers, ever**
  (`Co-Authored-By:`, `Generated with`, …), regardless of harness defaults.
- 🔴 **This repo is public. Job-application specifics never enter it.** Company names, posting text,
  submission deadlines, interview dates, and per-application scope plans stay **out of every tracked
  file** — including `AGENTS.md` and `docs/**`. The already-committed framing ("job-search artifact",
  §1/§4/§17 of the design doc) is the project's own purpose and is fine; a *particular* application
  is not. Time-boxed execution plans that carry such context live in `.wp/<slot>/` (gitignored in
  full — any filename, not just `work_*`). 🔴 **Never add a Nav row or any tracked link pointing at
  one** — the link is itself the leak. Their durable, non-identifying decisions get back-propagated
  into `docs/design/architecture-design.md` and the plan file stays untracked.
- **Secrets:** never log or commit `.env` / keys. Only `*.example` is committed.
- **Doc sync:** a change to behaviour / data / structure is incomplete until the affected doc is
  updated in the same change.

## Build
CMake root is `server/` (also the include root — `#include "atlas/core/types.h"`). Ninja generator
only: the VS generator emits no `compile_commands.json` and clang-tidy needs it. vcpkg manifest
(`server/vcpkg.json`, 7 deps, `builtin-baseline` pinned by `setup.bat`) + PCH + unity build.

| command | what |
|---|---|
| `server\setup.bat` | one-point installer — VS toolset → vcpkg clone/bootstrap → pin baseline → `npm ci` → configure. Idempotent. First run 20-60 min |
| `cmake --build --preset windows-debug` | unity ON — dev loop (run from `server/`) |
| `cmake --build --preset windows-ci` | unity OFF — missing-include / ODR gate |
| `ctest --preset windows-ci --output-on-failure` | tests (GoogleTest via `gtest_discover_tests`) |
| `powershell -NoProfile -File server\scripts\ci-gate.ps1` | **the full gate.** `-WhatIf` prints the plan |

Configure before building a fresh tree: `cmake --preset windows-debug` / `cmake --preset windows-ci`
(run from `server/`; `setup.bat` already does the `windows-debug` one).
Presets: `windows-debug` · `windows-ci` · `linux-release` · `linux-ci` (`{win,linux}-ci` = unity OFF).
CI gate order: `gen:check → core-purity → format-check → clang-tidy → build(unity ON) → build(unity
OFF) → test` (`ci-gate.ps1` inserts `configure` as prep before `format-check` — clang-tidy needs the
Ninja tree's `compile_commands.json`, so its printed step count is the list above plus that one).
🔴 The **non-unity** build is the gate that catches missing includes and ODR collisions — never skip it.
🔴 `gen:check` leads the gate: it is the mechanical enforcement of "never edit generated output".
🔴 `core-purity` follows it: the mechanical enforcement of "the core must not know the game" —
`server/atlas/**` may not include game-contract headers from `server/generated/**`, and must contain
zero terms from `tools/core_purity/denylist.txt`. The denylist grows with the demo game (`AD §3.3`).
`AD §15.4`.
🔴 **`clang-tidy` is CI-only (Linux/clang); `ci-gate.ps1` skips it and says so.** clang-tidy cannot
read the MSVC PCH the local Ninja/cl tree emits (`cmake_pch.cxx.pch` is a `/Yc` binary, not clang
AST). Suppressing the PCH just to satisfy the linter would tidy a TU the compiler never builds, so
the step moved to `linux-ci` instead. Undo when clang-cl lands locally. `AD §15.4` · `CS §7.3`.
cmake/ninja/clang-* are **not on PATH** — they ship with VS 2022; reach them via
`Common7\Tools\VsDevCmd.bat -arch=amd64` (both `setup.bat` and `ci-gate.ps1` do this themselves).
🔴 **CI is not currently proven green.** The last recorded run (`31531845296`, 2026-08-12) failed on
`clang-tidy` alone, and a red lint step leaves `build`/`test` **skipped** — a red that proves as
little as a hollow green. The 75 findings behind it are fixed (`AD §15.5i`); the next push is what
decides. Do not restate "CI is green" without a run id.
`ci-gate.ps1` is still the local gate and is **not** equivalent: it skips `clang-tidy` (MSVC PCH),
so naming / `bugprone` / `modernize` violations only surface in CI. `AD §15.5c`. A local
clang-tidy sweep **is** possible off a PCH-stripped copy of `compile_commands.json` and is a useful
pre-filter, but 🔴 it is deliberately not the gate: MSVC's STL never reports `operator*` for
`bugprone-unchecked-optional-access` while CI's libstdc++ does, so a clean local sweep is not
evidence. `AD §15.5i` · `CS §7.3`.
🔴 **`format-check` runs in both and can still disagree** — the local `clang-format` ships with VS
(19.1.5), CI's comes from `apt.llvm.org`; the 18.1.3 CI was really running measured `ColumnLimit` in
bytes, so a comment holding `§` or `—` passed locally and failed in CI. A green local gate is never
proof of a green CI. `AD §15.5f`.
🔴 **CI's `clang-tidy` had never actually run at 19** — `update-alternatives` silently loses to the
preinstalled 18 on `ubuntu-24.04`, so `.clang-tidy` failed to parse on every run and its check set
was never applied. Every step now calls the version-suffixed binary (`clang-tidy-19`) directly;
never reintroduce `update-alternatives` there. `AD §15.5g`.
🔴 The gate loads `server/.env` (key **names** logged, values never) so the DB suites actually run,
and **fails if a test was `Skipped` while a database was reachable** — `ctest` exits 0 on a skip, so
without this it printed PASS having proven nothing about the DB axis. CI has no MySQL service, so
**the DB axis is verified locally only.** `AD §15.4`.

## Context budget
Tool results are next-turn input tokens — the dominant cost. Bound output **at the source**, never by
dumping and skimming.

- **Read-only subagents are allowed and preferred** for broad exploration ("where is X", "map this
  dir", "what calls Y"). Their raw tool output dies in their own context; only the conclusion returns.
  🔴 Subagents that **write** (edit / commit / run builds) still require an explicit user request.
- Listing: filter before recursing — `-Directory`, a depth predicate, `Select-Object -First N`. A bare
  recursive file listing of this repo is never the right call.
- Searching: use the `Grep` tool with `head_limit` / `files_with_matches`, not shell `grep`/`rg`.
- Reading: use `Read`. For a large doc, `Grep` the headings first, then `Read` that range only.
- Git: `-1` / `-n N` / `--oneline` / `--format=…` / `--stat`. (`git diff` for a commit is exempt.)
- Never re-run an identical search or re-read a file already in context.

## Agent tooling
| path | what |
|------|------|
| `.claude/skills/plan-work/` | finalized design → dependency DAG of self-contained `work_prompt{N}.md` + `work_plan.md`. Planning session only; writes no code |
| `.claude/skills/run-work/` | reads `work_plan.md`, dispatches ready nodes to subagents, re-runs each node's verify itself before flipping it to `done`. Fresh execution session |
| `.claude/commands/git-commit.md` | `{type}: {한글 요약}` commits, grouped by logical work unit. No issue numbers |
| `.claude/hooks/sot-router-reminder/` | `UserPromptSubmit` — re-arms the Domain matrix + Mandates each turn. Carries behaviour only, never restates the tables (they would drift). Disable: `ATLAS_SOT_ROUTER_DISABLE=1` |
| `.claude/hooks/bulk-output-guard/` | `PreToolUse(Bash\|PowerShell)` — denies unbounded recursive listings, whole-file `cat`, shell content search, and unbounded `git log`; points at the bounded tool. Enforces "Context budget" above. `git diff` exempt. Disable: `ATLAS_BULK_OUTPUT_GUARD_DISABLE=1` |
| `.claude/settings.json` | hook registration. `settings.local.json` is gitignored |
| `.agents/skills/*/` | Codex bridge stubs — 5-line pointers at the `.claude/skills/*` originals + `agents/openai.yaml` display metadata. **Never fork the workflow here**; edit the `.claude` file |
| `tools/` | **Node data-pipeline = the templatization seam** (`AD §14`). `config-loader.js` + `types.json` (normalized-type SoT, `cpp` column = the C++ seam) + `all_generator.bat`, and **all three** generators `{info,pkt,db}_generator/`. Driven from the repo root by `npm run gen:*`. See `tools/AGENTS.md`.<br>`tools/loadreport/` is **not** a generator — it renders a load run's JSONL into a self-contained HTML (`npm run loadreport`, `AD §16.1a`). 🔴 It is deliberately outside `gen:all`/`gen:check`: its input is a measurement, not a source, so drift against it is meaningless |
| `template.ini` · `package.json` | generator paths / targets / namespaces (🔴 never secrets), and the `gen:all` · `gen:check` · `gen:db:apply` script contract — **`info → db → pkt`**, input data before schema before packets (`AD §14`). 🔴 `gen:db:apply` is **dev-only**: it exits 1 before opening a socket unless `ATLAS_ENV=dev`, because prod migrations are applied by a human from `server/db/migrations/` (`AD §10.7`). Its live-DB path lazily `require`s `mysql2`, which is a declared `devDependency` — never leave it undeclared, or a fresh clone emits no migration and still exits 0. 🔴 Only wire a `gen:*` script for a generator this repo **actually ships or has planned work to create** — a script pointing at one nobody will write pins `gen:check` at exit 1 forever and kills the drift gate. With the third generator landed, what this rule now guards against is pre-wiring a **fourth** |

**Repo layout**
```
server/    C++ framework + CMake root + setup.bat + server.ini + .env.example
           + db/schema.json + generated/{info,pkt,db}
shared/    contracts/ (*.cs → pkt input) · datas/ (*.csv → info input)
tools/     3 generators (Node: info, pkt, db) + config-loader.js + types.json
docs/  template.ini  package.json  compose.yaml     (client/ is a later slice)
```

DAG artifacts live in `.wp/<slot>/` and are gitignored (`work_plan.md`, `work_prompt*`).

Not yet built (deferred until code exists, because their target paths and build commands must be
real): a generated-output write guard, a `clang-format`/`clang-tidy` `PostToolUse` hook, a
`gen-check` CI drift gate, and the per-layer implementation skills.

## Clarification Protocol
Stop and ask **before** implementing ONLY when: the requirement is ambiguous with design impact, a
clearly better alternative exists, or the task touches protocol / auth / cross-server contracts.
Format: `QUESTION: [what] | OPTIONS: A) … B) … | RECOMMEND: [A/B] — [reason]`
