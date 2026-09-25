---
name: session-archaeology
description: >
  Provides a PPR-specific execution extension when Reflect is requested for
  session archaeology across all OpenCode sessions, while preserving Reflect's
  workflow.
---

# Session Archaeology

## Scope and authority

This is a thin, repository-local execution extension for Reflect. It is not a
second Reflect workflow and does not replace the general Reflect guidance.

Before any analysis, load and follow the original global `reflect` skill. If
that original skill is unavailable, stop with a clear message: `The original
global reflect skill is unavailable; session archaeology cannot run.` Do not
duplicate or improvise its workflow here.

The original `reflect` skill remains authoritative for inventory, evidence
selection, confidence, conservative recommendations, report format, and
proposal-before-change behavior. Apply this extension only after that skill
has established the task and scope.

## SQLite fast path

Use the following fast path only for the session evidence Reflect has selected:

1. Resolve the OpenCode database portably. Honor `OPENCODE_DB` when set.
   Otherwise resolve the normal user-local OpenCode data location for the
   current platform: on Windows, use the platform user-data variables and
   home directory; on Unix, use `XDG_DATA_HOME` when set and otherwise
   `HOME`. Never embed a machine-specific absolute path as repository truth.
2. Open the database through SQLite read-only URI mode `mode=ro`. Use short
   transactions and set `busy_timeout` so a live database is not held for a
   long operation. Warn against `immutable=1` for the live WAL database: WAL
   state can make that assumption unsafe.
3. Start with Tier 0 session metadata only. Read the session inventory and
   bounded metadata fields needed by Reflect; do not read message or other
   JSON-bearing values during this pass.
4. Tier 1: When JSON-bearing tables must be considered, perform at most one scan per
   such table where practical. Do not use bulk JSON or text selection. Keep
   any derived signal bounded and non-sensitive.
5. Use Tier 2 only for a bounded, indexed drill-down of the sessions or rows
   selected by Reflect, with a strict limit. Do not scan an entire database
   for an individual session.
6. Keep the watermark and cache identity outside the database. They belong to
   local execution state, not to OpenCode session tables or session artifacts.

## Safety boundaries

- Do not ask an LLM to summarize each session individually. Reflect's evidence
  and recommendation contract is authoritative.
- Do not bulk-select JSON or text, read sensitive `message`, `part`, `tool`,
  `todo`, or `session_input` values, or inspect credential/account tables.
- Do not write FTS data, create indexes, change schema, or otherwise mutate the
  live OpenCode database.
- Emit metadata-only, redacted evidence. Do not include private content in the
  report or in generated recommendations.
- Do not commit session data, cache, or report artifacts. `.slim/tmp/` may be
  used only for ephemeral local scratch and must remain untracked; it is not a
  deliverable output location.
- Keep the scope repository-local and machine-portable. This skill adds no
  repository configuration, database schema, global setting, or other file
  change.
