---
name: plan-work
description: Decompose a confirmed design into a dependency DAG of self-contained work_prompt files and a work_plan manifest. Use for /plan-work, work decomposition, or DAG planning requests.
---

# Shared workflow bridge

Read `../../../.claude/skills/plan-work/SKILL.md` completely and follow it as the canonical workflow.

Map Claude-specific tool names to equivalent Codex tools. Treat text following an explicit skill
invocation as `$ARGUMENTS`. Map subagent concepts to Codex collaboration tools only when the user has
authorized delegation. If an instruction has no safe Codex equivalent, report that incompatibility
instead of inventing behavior.
