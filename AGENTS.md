# project-atlas

**C++ MMO game-server framework.** Not a game — a reusable server core that any client can be
swapped onto. The demo game exists only to exercise the core.

**Primary purpose = job-search artifact**: prove C++ / async IO / TCP / serialization /
multithreading / load at scale. 🔴 **Revenue is explicitly not a goal of this project.**

Status: **design complete, implementation not started** (2026-08-06).

> Doc language: `AGENTS.md` is English and token-efficient (AI context). Human design docs under
> `docs/` are Korean. Chat replies follow the user's language preference.

## Nav
| path | role | doc |
|------|------|-----|
| `docs/design/architecture-design.md` | **Top-level SoT.** Purpose, Phase-1 success criteria, topology (FE/GAME/WORLD), 3-tier identity, Actor/AoI/BT, protocol + integrity layers, thread model + ctx, custom ORM, logging/exception policy, templatization seam, build chain, open items, risks | — |
| `docs/conventions/cpp-style.md` | **Coding convention SoT.** Namespace, naming, type rules (fixed-width ints, A-plan aliases, strong-typed IDs), macro policy, template policy, and the 3-layer mechanical enforcement | — |

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
- **Secrets:** never log or commit `.env` / keys. Only `*.example` is committed.
- **Doc sync:** a change to behaviour / data / structure is incomplete until the affected doc is
  updated in the same change.

## Build
CMake (SoT) + PCH + unity build (optional) + vcpkg manifest (pinned baseline) + multi-stage Docker.
CI gate: `format-check → clang-tidy → build(unity ON) → build(unity OFF) → test`.
🔴 The **non-unity** build is the gate that catches missing includes and ODR collisions — never skip it.

## Clarification Protocol
Stop and ask **before** implementing ONLY when: the requirement is ambiguous with design impact, a
clearly better alternative exists, or the task touches protocol / auth / cross-server contracts.
Format: `QUESTION: [what] | OPTIONS: A) … B) … | RECOMMEND: [A/B] — [reason]`
