---
name: run-work
description: Read the work_plan.md DAG and execute ready nodes. Parallel groups are dispatched to subagents concurrently; each node is gated by an independently re-run verify before its status flips to done and unblocks dependents. Runs in a FRESH execution session. Triggers on "/run-work", "execute the work", "run the wp files", "run the DAG", or Korean equivalents (작업 실행 / wp 실행 / DAG 돌려).
---

# run-work — DAG execution orchestrator

**Role:** read the `work_plan.md` produced by `plan-work` and execute nodes whose dependencies are
satisfied. Parallel nodes are dispatched to **subagents (separate context windows)** to keep this
session's context thin.

**This session is the orchestrator.** Do NOT implement node bodies here — delegate to subagents and
absorb only their final result + verify. (Cold-start cost is accepted; isolation is the point.)

## Workspace slot (`--slot <name>`)
Must match the slot `plan-work` wrote to:
- `--slot <name>` → read/write `.wp/<name>/work_plan.md` + `.wp/<name>/work_prompt{N}.md`.
- No `--slot` → repo root `./` (legacy default).

Every `work_plan.md` / `work_prompt{N}.md` reference below (manifest load, status writeback, subagent
prompt) is **relative to this slot dir**. Prepend `.wp/<name>/` to every wp path you hand a subagent
when a slot is set.

## Arguments (`$ARGUMENTS`)
- `--slot <name>`: workspace slot (see above). May combine with any argument below.
- empty or `--all`: run the whole DAG to completion.
- `wpN` (e.g. `wp3`): run only that node (if its deps are unmet, stop and report).
- `--from wpN`: run from that node to the end.
- `--status`: report current node states without executing.

## Procedure

### 1. Load manifest + recover
- Read `work_plan.md` (in the slot dir if `--slot` set, else root). If missing, stop and advise:
  "no work_plan.md in <slot-or-root> — run `plan-work` (with the same `--slot`) in a planning session
  first."
- Parse the node table: id / layer / depends_on / parallel_group / writes / status / file.
- **Orphan `running` recovery:** if a prior session died leaving nodes at `status=running`, there is
  no completion evidence — reset them to `pending` (re-runnable). Leaving them stuck (neither pending
  nor done) deadlocks dependents forever. Give the user a one-line notice before resetting.

### 2. Compute the Ready set
- **Ready** = `status=pending` AND every `depends_on` is `status=done`.
- No Ready node, unfinished nodes remain, all blocked → stop and report the blocking cause.
- All done → report completion and exit.

### 3. Execute — parallel dispatch
- **Parallelism is decided by the Ready set, NOT the `parallel_group` label.** Among currently-Ready
  nodes, take those whose `writes` are **mutually disjoint** and dispatch them by calling the Agent
  tool **multiple times in one message** (concurrent).
- If two or more Ready nodes have **overlapping `writes`** → do NOT parallelize; run them **one at a
  time** (prevents lost writes). `plan-work` may have missed the serialization; defend here.
- A single Ready node → one subagent.
- Just before running a node, set its status to `running` in `work_plan.md`.

**Prompt to pass each subagent (must be self-contained):**
```
You are the executor for node <wpN>.
1. Read <slot-path>work_prompt{N}.md (frontmatter + body = your complete instructions).
   [<slot-path> = `.wp/<name>/` if a slot is set, else empty.]
2. Read the root AGENTS.md for project rules, Mandates, and generator names. If the node touches
   code, also read docs/conventions/cpp-style.md in full.
3. Execute the "Instructions" exactly. For anything unspecified, follow AGENTS.md — do not guess.
   - Do NOT modify files outside your `writes` set. NEVER touch work_plan.md or other wp files
     (status is the orchestrator's job alone).
   - NEVER edit generated output. Change the source (CSV / schema.json / contracts) and re-run the
     generator.
4. Actually run the "verify" (generator / build / rg). Do not report a result you did not observe.
5. Final report: list of changed files + verify result (PASS/FAIL) + cause if FAIL.
```

### 4. Verify gate (consistency safeguard — replaces human review in unattended runs)
After a subagent returns:
- **The orchestrator re-runs verify itself to confirm** — do not trust the subagent's self-reported
  PASS (implementer == verifier can rationalize). Gate nodes (`design` / `data` / `proto` / `db` /
  `tool`) especially require independent re-run, because every downstream node inherits their output.
- Only a re-run **PASS** flips the node to `status=done` in `work_plan.md` → unblocks dependents.
- **FAIL** → `status=blocked`, **halt DAG progress**. Include the **list of partially-changed files**
  in the report (no git tracking = no rollback; the user must judge the dirty state). Do not silently
  work around or retry-loop (may be a design problem needing a human).

**Mandate re-checks the orchestrator runs itself** (cheap, and a subagent has an incentive to skip
them):
- `rg -n "#pragma pack"` over the node's `writes` → must be 0
- `rg -n "\b(int|long)\b"` over protocol / persistence / ID paths in `writes` → must be 0
- no generated-output path appears in the diff unless the node's `layer` is `tool`
- once the build chain exists: **both** `build(unity ON)` and `build(unity OFF)` — the non-unity
  build is the gate that catches missing includes and ODR collisions; never skip it

### 5. Loop
- Recompute Ready → repeat 3–4. For `--all`, continue until blocked or all done.
- In single `wpN` mode, process just that one node and exit.

### 6. Final report
Compact table: per-node status / changed-file count / verify. If anything is blocked, put the cause
at the top.

## Rules
- Keep the orchestrator context lean: do not absorb subagents' raw tool output — only their final
  result message.
- Coupled nodes (proto ↔ CSV, db ↔ ORM call sites) should already be merged by `plan-work`. If two
  coupled nodes appear with overlapping writes in the Ready set → halt, warn "plan-work needs
  re-review."
- Write `status` back to `work_plan.md` immediately on each node's completion (so a dropped session
  can resume).
- Never flip a FAIL to done. Ever.
- Doc-sync: verify that a node's body included its doc update. A missing doc update counts as FAIL.
