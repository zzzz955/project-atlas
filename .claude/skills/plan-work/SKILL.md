---
name: plan-work
description: Decompose a confirmed design into a dependency DAG of self-contained work_prompt{N}.md nodes plus a work_plan.md manifest. Runs in the PLANNING session (the one where the design was finalized). Triggers on "/plan-work", "decompose the work", "generate wp files", "build the DAG", or Korean equivalents (작업 분해 / wp 생성 / DAG 짜줘).
---

# plan-work — design → work DAG generator

**Role:** turn an already-finalized design into **self-contained work nodes** that a fresh execution
session can run, some in parallel via subagents. This skill only writes files: `work_prompt{N}.md`
nodes + `work_plan.md` (the DAG manifest). It does **not** implement anything.

**Session boundary:** run this in the **planning session** (same session that finalized the design,
so the design context is still loaded). Execution is a separate concern handled by `run-work` in a
fresh session.

## Workspace slot (`--slot <name>`)
Multiple planning sessions can own separate DAGs at once, so all output lives in a **slot directory**
to avoid clobbering another session's in-flight wp files:
- `--slot <name>` given → write to `.wp/<name>/` (create if absent). Use a task-descriptive name
  (e.g. `fe-session-skeleton`).
- No `--slot` → write to the repo root `./` (legacy default — only safe when no other session owns
  the root `work_prompt*` / `work_plan.md`).

Every `work_prompt{N}.md` / `work_plan.md` path below is **relative to this slot dir**. Overwrite
logic (§5) applies only within the slot — a slot never touches another slot's or the root's files.
Tell the user to invoke `run-work` with the **same** `--slot <name>`.
(`.gitignore` already ignores `work_plan.md` + `work_prompt*` at any depth → slot dirs are untracked;
no gitignore change needed.)

## Preconditions (if unmet, STOP and ask)
- Is the design actually finalized? If not, do not decompose — ask back using the `QUESTION:` format
  from `AGENTS.md`.
- Do you know each artifact's Source-of-Truth? For this repo that is
  `docs/design/architecture-design.md` (architecture) and `docs/conventions/cpp-style.md` (code
  style), plus the generator inputs (CSV / `schema.json` / contracts).

## Procedure

### 1. Absorb context
- Read `AGENTS.md` (root; plus any nested one under the target directory). Extract: the three
  generator logical names (`pkt_generator` / `db_generator` / `info_generator` — repo-global
  standard), the wrapper command that runs them, pipeline order, the Mandates list, the build chain,
  and the doc-sync obligation.
- Prefer already-loaded `AGENTS.md` context; only read files when it is absent.
- 🔴 The Mandates in `AGENTS.md` are **not advisory** here — §5's `verify` must encode the ones the
  node can actually violate (see §5b).

### 2. Layer gating (NO rigid template — emit only the nodes the work needs)
Judge each gate **independently** against the work. If a gate does not apply, do not create that node.

| Gate | Include node when | `layer` value |
|------|-------------------|---------------|
| Tool | a generator itself or a build/dev script changes | `tool` |
| Design doc | behaviour / data / structure changes → `docs/**` must update | `design` |
| Static data | CSV numbers/tables added or changed (`info_generator` source) | `data` |
| Contract | packet / DTO / enum / protocol changes (`pkt_generator` source) | `proto` |
| DB | persistent schema changes (`schema.json`, `db_generator` source) | `db` |
| Core | shared core: net/strand, serialization runtime, logging, ORM runtime, common types | `core` |
| FE | FE server — connection termination, routing, WORLD registry, heartbeat | `fe` |
| GAME | GAME server — persistence, character, inventory, growth, mail | `game` |
| WORLD | WORLD server — coordinates, combat, rooms, Actor, AoI, BT, tick loop | `world` |
| Build | CMake targets, vcpkg manifest, Docker, CI workflow | `build` |

Example: "add a heartbeat timeout knob to FE only, no packet change" → `fe` + `design`, two nodes.
`proto` / `db` / `data` / `core` / `build` all skipped. The DAG collapses to 2 nodes.

### 3. Wire dependencies + wave placement
- **Standard waves** (wire only among nodes that actually exist):
  - **Wave A:** `tool` — a generator change is upstream of every artifact that generator emits.
  - **Wave B (parallel):** `design` / `data` / `proto` / `db` — the SoT for everything downstream.
    If independent, share a `parallel_group`.
  - **Wave C:** `core` — depends on Wave B (generated types land here).
  - **Wave D (parallel):** `fe` / `game` / `world` — depend on `core` and on their own Wave-B nodes.
  - **Wave E:** `build` — only when new source files / targets / deps appeared.
- `depends_on` may reference **only node ids that were actually created**. Never point at a
  non-existent node.

### 4. Merge coupled nodes (consistency safeguard — mandatory)
Artifacts that cross-reference each other (where consistency can break) must be **one node**. Never
split them across parallel subagents.
- `proto` (packet/DTO) ↔ the CSV/enum that uses it → single node.
- `db` (`schema.json`) ↔ the ORM call sites that depend on the emitted CRUD signature → make `db` an
  upstream `depends_on`, never parallel.
- 🔴 **Cross-server routing is never split.** A change to how FE routes to GAME/WORLD touches both
  sides of one contract — put the contract in a `proto` node and make **both** server nodes depend
  on it. WORLD ↔ WORLD direct communication is forbidden; if a decomposition implies it, the design
  is wrong — STOP and ask, do not emit the node.
- Test: "if two different cold sessions build these without seeing each other, do they diverge?"
  Yes → merge.

### 4b. Prevent parallel write collisions (mandatory)
Nodes that can run concurrently (i.e., no dependency between them) must have **disjoint write-sets**.
Overlap → lost writes.
- Declare every path a node modifies in the frontmatter `writes:` field.
- If two nodes write the **same file** (especially a shared `AGENTS.md`, a common core header, or
  `docs/design/architecture-design.md`) → **serialize** them via an artificial `depends_on`, or merge.
- Common trap: several nodes each doc-sync the same SoT doc. → concentrate the doc update in one
  node, or force ordering.
- Common trap here: `fe` / `game` / `world` nodes each adding to the same shared core header. →
  move that edit into the `core` node and make the three depend on it.

### 5. Write node files — `<slot>/work_prompt{N}.md`
Write into the slot dir (§ Workspace slot). If `work_prompt*` files already exist **in this slot**,
**overwrite** them (never touch files outside the slot). Each file's top frontmatter must match this
schema exactly:

```
---
id: wp3
layer: proto
depends_on: []               # array of upstream node ids. Empty = Ready immediately.
parallel_group: waveB        # human-readable label ONLY. Real parallelism = Ready(deps done) + non-overlapping writes. The label never overrides deps.
writes: [<contract_file>]    # paths/globs this node modifies. Used for collision detection.
verify: "<generator re-run clean> && <no #pragma pack> && <no bare int/long in protocol layer>"
status: pending
---
```

Below the frontmatter, the body = **self-contained instructions**. A cold subagent must be able to
execute from this file + `AGENTS.md` alone. It must include:
- **Background / confirmed spec**: leave no room to guess. Spell out numbers, naming, paths, and the
  SoT section number it came from (e.g. `architecture-design.md §7.2`).
- **Instructions**: numbered steps. Name the exact files to touch.
- **Verify**: expand the frontmatter `verify` into concrete commands/checks.
- **Doc-sync**: `AGENTS.md` requires it — a behaviour/data/structure change is incomplete until the
  affected doc is updated in the **same** node. Absorb the doc update into the node; do not spin it
  off as a separate node (that creates the §4b shared-file collision).

### 5b. Deriving `verify` (this repo)
**Never invent a build command.** Derive it from `AGENTS.md`. If `AGENTS.md` does not document how to
build or verify that layer, ask via `QUESTION:` — a wrong verify is a consistency hole.

Current repo state: **implementation not started**, so there is no CMake target to build yet. Until
`AGENTS.md`'s Build section names a real wrapper command, a node's verify is allowed to be
**document-and-grep level**, and must say so explicitly rather than pretending a build ran:

| Node kind | verify (pre-implementation) | verify (once the build chain exists) |
|---|---|---|
| `design` | affected `docs/**` section updated; cross-references consistent; `rg "<old_term>"` = 0 | same |
| `data` / `proto` / `db` | source file (CSV / contract / `schema.json`) is well-formed and self-consistent | run the generator wrapper → build passes → `git diff --exit-code` on generated output (drift gate) |
| `core` / `fe` / `game` / `world` | N/A — do not emit an implementation node before the build chain exists unless the user explicitly accepts an unbuildable node | `build(unity ON)` **and** `build(unity OFF)` **and** `test` |
| `build` | CMake/vcpkg/CI file parses | full CI gate: `format-check → clang-tidy → build(unity ON) → build(unity OFF) → test` |

**Mandate greps** — include the ones a node can actually violate, as literal commands:
- `rg -n "#pragma pack" <writes>` → must be 0
- `rg -n "\b(int|long)\b" <protocol|persistence|ID paths>` → must be 0 (fixed-width aliases only)
- generated-output paths must not appear in the node's `writes` (change the source, re-run the
  generator)
- async handler entry points wrapped in `Guarded`

### 6. Write the manifest — `<slot>/work_plan.md`
The whole DAG in one file. `run-work` reads this to schedule. Format:

```
# Work Plan — <task title>
generated: <YYYY-MM-DD>
legend: pending | running | done | blocked

## Nodes
| id | layer | depends_on | parallel_group | writes | status | file |
|----|-------|-----------|----------------|--------|--------|------|
| wp1 | design | - | waveB | docs/design/architecture-design.md | pending | work_prompt1.md |
| wp2 | proto | - | waveB | <contract_file> | pending | work_prompt2.md |
| wp3 | core | wp1,wp2 | waveC | <core_glob> | pending | work_prompt3.md |
| wp4 | fe | wp3 | waveD | <fe_glob> | pending | work_prompt4.md |
| wp5 | world | wp3 | waveD | <world_glob> | pending | work_prompt5.md |

## Waves (human-readable)
- Wave B (parallel — no deps + disjoint writes): wp1, wp2
- Wave C: wp3 ← [wp1,wp2]
- Wave D (parallel): wp4, wp5 ← [wp3]
```
> wp4 and wp5 are parallel-safe only because their `writes` are disjoint. If both must touch the same
> core header, that edit belongs in wp3 (§4b).

### 7. Report the DAG to the user
After writing files, report node count / wave structure / where parallelism is possible, as a compact
table. Then point them onward:
> Execute in a fresh session with `run-work`, passing the **same slot**:
> `/run-work --slot <name> --all` (auto-parallel) or `/run-work --slot <name> wp3` (single node).
> (No slot was used → omit `--slot`.)

## Rules
- No execution. Do not touch code / data / docs. Only create the wp + plan files.
- Do not over-decompose. If splitting yields no real parallel gain, combine. If a wave exceeds 3–5
  nodes, re-check for merges.
- If the decomposition would require WORLD ↔ WORLD direct traffic, or expanding the demo game's
  minimum set (design doc §3.3), STOP and ask — those are Mandates, not node-level decisions.
