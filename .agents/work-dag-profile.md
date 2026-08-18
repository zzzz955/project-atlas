---
profile_id: project-atlas
profile_version: 2
core_major: 3
default_slot: root
repo_root_rule: git
exclusive_resources: []
---

## Context

Read root `AGENTS.md`, its triggered domain references, affected nested instructions, and
`docs/conventions/cpp-style.md` for code nodes. Architecture design and style docs are authoritative.

## Layers

Use `tool` → (`design`, `data`, `proto`, `db`) → `core` → (`fe`, `game`, `world`) → `build`.
Emit only applicable gates.

## Coupling

Keep proto with dependent enums/serialization; DB schema precedes generated ORM call sites; shared
headers belong to `core`. A cross-server route uses one contract upstream of both endpoints. WORLD
to WORLD direct traffic is forbidden and requires design correction.

## Verification

Enforce fixed-width protocol/persistence types and no `#pragma pack`. When the build chain exists,
run format/tidy plus both unity ON and OFF builds and tests as documented; never invent a target
before it exists.

## Doc sync

Update affected architecture/design and instruction files in their owning nodes.
