---
name: run-work
description: Read the work_plan manifest and execute ready DAG nodes, gating each on an independently re-run verify. Use for /run-work, DAG execution, or work_prompt execution requests.
---

# Shared workflow bridge

Read `../../../.claude/skills/run-work/SKILL.md` completely and follow it as the canonical workflow.

Map Claude-specific tool names to equivalent Codex tools. Treat text following an explicit skill
invocation as `$ARGUMENTS`. Map subagent concepts to Codex collaboration tools only when the user has
authorized delegation — if delegation is unavailable, run nodes sequentially in this session and say
so; never claim parallel execution that did not happen. If an instruction has no safe Codex
equivalent, report that incompatibility instead of inventing behavior.
