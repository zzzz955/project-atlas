#!/usr/bin/env node
'use strict';

// project-atlas SoT Router Reminder Hook
// Runs on every UserPromptSubmit and injects a SHORT re-classification nudge
// via stdout (delivered as a system message).
//
// Design:
// - The Nav table, the Domain matrix, and the Mandates live in ONE place:
//   the repo-root AGENTS.md (loaded as session instructions). This hook
//   deliberately does NOT restate them — re-injecting the data every turn both
//   drifts from AGENTS.md and taxes the context budget.
// - What must survive every turn is the *behavior*, not the data: re-classify
//   the new message, load the missing SoT section before planning, honour the
//   Mandates, doc-sync before claiming done.
// - "In context" semantics: SoT sections must be PRESENT in context, not
//   re-read per turn. Re-reading files already in context wastes budget.
//
// Coupling: the nudge names the "Nav", "Domain matrix", "⚖️ Mandates" and
// "Clarification Protocol" sections of AGENTS.md — keep in sync if those
// headings change.
//
// Disable for debugging: ATLAS_SOT_ROUTER_DISABLE=1

const fs = require('fs');

if (process.env.ATLAS_SOT_ROUTER_DISABLE === '1') {
  process.exit(0);
}

try {
  fs.readFileSync(0, 'utf8');
} catch (_) {
  /* stdin is advisory only */
}

process.stdout.write(
  '<atlas-sot-router-reminder>\n' +
  'NEW user message — re-classify it against the Domain matrix in AGENTS.md BEFORE you\n' +
  'plan, search, or edit. AGENTS.md is the single source of truth for routing; this\n' +
  'reminder only re-arms it each turn.\n' +
  '\n' +
  '1. SoT IN CONTEXT. Before planning or editing, the AGENTS.md-matched section of\n' +
  '   docs/design/architecture-design.md (architecture) and/or\n' +
  '   docs/conventions/cpp-style.md (code style) must be present in context. Load what\n' +
  '   is missing this session; do NOT re-read what is already fully in context.\n' +
  '2. Re-classify EVERY turn. A section loaded in a previous turn does not exempt this\n' +
  '   one. If this message touches a new domain row, load that row\'s doc BEFORE\n' +
  '   planning. Multiple rows -> load all.\n' +
  '3. Mandates are zero-tolerance. Re-read the "⚖️ Mandates" section of AGENTS.md if it\n' +
  '   is not in context. The ones most often violated silently: never edit generated\n' +
  '   output (change the CSV / schema.json / contract and re-run the generator); no\n' +
  '   bare int/long in protocol, persistence, or ID layers; no #pragma pack on packet\n' +
  '   structs; no WORLD <-> WORLD direct traffic; no exception escaping an async\n' +
  '   handler (Guarded).\n' +
  '4. Clarification Protocol. If the request is ambiguous with design impact, a clearly\n' +
  '   better alternative exists, or it touches protocol / auth / cross-server\n' +
  '   contracts -> ask FIRST in the QUESTION | OPTIONS | RECOMMEND format. Do not\n' +
  '   implement and ask afterwards.\n' +
  '5. Doc-sync before "done". A behaviour / data / structure change is incomplete until\n' +
  '   the affected doc is updated in the SAME change. If the code contradicts a doc,\n' +
  '   the code is truth — fix the doc.\n' +
  '\n' +
  'Self-check: if you cannot name which Domain matrix rows fired for THIS message, you\n' +
  'have not re-classified — go back to AGENTS.md now.\n' +
  '</atlas-sot-router-reminder>\n'
);
