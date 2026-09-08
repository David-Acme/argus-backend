# Task F6-3b batch 2 report — rule 20 sweep over src/feature, src/filter, src/test

Branch `f6-3b-batch2` (isolated worktree /tmp/f6-3b-batch2, branched from master
HEAD 0ac8926, merged batch 1). Scope per the controller's scope change: only
`src/feature/**`, `src/filter/**`, `src/test/**` — `src/main.cc` and
`src/config/**` were already deleted from the tree by F6-4 and were skipped, as
was every other inventory line whose file no longer exists.

## Result

57 files touched, +43 / -448. Comment-only change, zero behavior change, no
constexpr introduced (this batch's ruling 3 variant: delete the comment only).

## What was done

- Condensed multi-line doc blocks above classes/structs/functions to single
  short lines (sync-forwarder.hxx, sync-service.hxx, the six feature-service
  headers' private-emit docs, auth-service.cc/.hxx, device-filter.cc/.hxx,
  jwt-filter.hxx, and the test-file helper/class docs: llm-wire-test,
  memory-backpressure-test, memory-remote-adapter-test, memory-wire-test,
  notification/productivity-controller-test, stt/tts/vlm/vision tests,
  golden-sync-test, the four fake-*-server.hxx fixtures,
  identity-client-test, camera-controller-test, cert-san-test,
  device-credential-test, vlm-wire-test).
- Deleted all in-function / per-statement comments (bucket B): step
  narration in wire tests, controller-test assertions narration, DTO field
  comments (create-zone-dto.hxx, device-login-status-dto.hxx trailing
  `// pending | approved | expired`), fake-server option field comments.
- Deleted all test section banners (`// ── ... ──`, ruling 4): llm-wire,
  memory-wire, memory-remote-adapter, memory-replica, stt-wire, tts-wire,
  vlm-wire, camera-controller, camera-talk-cutover, productivity-controller.
- Deleted every "Ruling X" token comment entirely (ruling 5): BX in
  user-feature-service/auth-service, BW in memory-replica-test, BE in
  stt-wire-test, BZ in memory-backpressure-test, Y/AR in
  notification/productivity-controller-test, AM in productivity-controller-test.
  Also deleted phase tokens: "F6-4" in event-sync-empty-test.cc, "F4-2 lesson"
  in voice-llm-remote-test.cc and voice-stt-remote-test.cc, "rule 6" reference
  in camera-operator-test.cc.
- Deleted the proven-stale camera-controller-test.cc banner + legacy prose
  (bucket E, brief ruling 9: no CONTEXT home needed).
- Kept the 47 `// namespace` end tags (ruling 1) and the five one-line
  `/** ... */` function docs (bucket F).

## CONTEXT.md re-homing

None needed: every candidate rationale was already documented in root
CONTEXT.md (sqlite SQLITE_CONFIG_MULTITHREAD ordering at lines 40-46/1862-1868;
vision FNV-1 cache-key parity at lines 2610-2626; the event-domain empty-shape
no-fallback at lines 2825-2827). CONTEXT.md is untouched by this commit.

## Verification

- Scripted diff classification (python over `git diff`): OK — only full-line
  comment removals, `//`-prefixed line edits, one trailing-comment-only line
  edit (device-login-status-dto.hxx, the ruling-3 shape), and blank-line-free
  deltas. Zero code-line changes. CONTEXT.md: no diff.
- Post-sweep scans: zero remaining multi-line comment blocks, zero
  statement-attached or trailing comments, zero Ruling/Fase/Fn-N tokens, zero
  TODO/FIXME/commented-out code in scope.
- EOF gate: all 57 touched files end with `\n` (tail -c1 = 0a). No double
  blank lines introduced. All added/kept comments 100% English.
- No build run, per batch instructions (comment-only change, hook-verified).

## Concerns / notes for the reviewer

- fake-llm-server.hxx's `FakeLlmOptions` lost its two field one-liners
  (`coalesce` / `truncated`). Renaming the fields to be self-documenting would
  have been the rule-20 rewrite, but it would have changed code lines, which
  this batch's verification hook forbids. The semantics are still visible in
  the consuming assertions (llm-wire-test, voice-llm-remote-test).
- Batch 1 precedent kept some single-line field-attached docs; this batch
  deletes them where the statement survives without them, per the strict
  bucket-B default. Flagging in case the reviewer wants uniformity.
- Inventory lines for deleted files (src/config/application.cc, src/main.cc)
  were skipped; inventory line numbers drifted on cross-imported files, so all
  edits were made against current file state.