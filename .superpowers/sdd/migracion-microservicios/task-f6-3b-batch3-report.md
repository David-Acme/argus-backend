# Task F6-3b — batch 3 report (argus-* service trees except argus-gateway/argus-voice)

Scope: `argus-camera/` (incl. its labs), `argus-productivity/`, `argus-notification/`,
`argus-llm/`, `argus-vlm/`, `argus-tts/`, `argus-stt/`, `argus-memory/` — 60
`.cc`/`.hxx` + 5 service CONTEXT.md files touched. HEAD f7a04e0; comment-only sweep.

## Verification

- `git diff` gate script (string-safe): `added_nonblank=69 removed=313
  added_blank=1 constexpr=2 BAD=0` — every added code-bearing line is a
  `//`-comment or one of the 2 allowed constexpr lines; all removals are
  comment-only lines.
- Multi-line comment blocks remaining in touched files: 0. `/*` block comments
  remaining in the whole batch: 4, all single-line function/class docs that the
  inventory keeps (bucket F).
- `Ruling`/`Appendix`/`Fase` tokens in code: 0.
- EOF newline gate (`tail -c1` = 0a) on all 60 touched sources: 0 violations.
- No builds run (per brief).

## Counts per bucket applied (my scope)

- A multi-line doc/narrative blocks: 47 condensed to one-liners (A.1: 22,
  A.3: 25) — includes the 3 main.cc boot-capacity blocks, the 6 change-funnel
  blocks, controller/DTO/struct blocks, and the 2 lab file headers.
- B single-line in-function / per-statement comments: 41 deleted (incl. the
  8 event-intelligence rule-number step comments, detector shape comments,
  lab step comments, identity-wait and CORS/foreign_keys pre-statement notes).
- C commented-out code: 0 (verified, matches inventory).
- D trailing comments: 1 (`nats-object-event-sink.cc:52` "7 days, in
  nanoseconds") → new `constexpr int64_t kJetStreamRetentionNs` at file scope,
  comment deleted, value expression byte-identical.
- E stale: 2 of the 4 proven stale comments fell in this batch and are deleted
  (`argus-tts/src/main.cc` and `argus-stt/src/main.cc` "serving 503s to the
  legacy adapters" — the legacy registers no TTS/STT adapter); the other 2
  (`db-service.hxx`, `camera-controller-test.cc` banner) are batch 1/2 files.
- F kept: the compliant one-liners above class/function declarations, all
  `// namespace` end tags, and the one-line `/** */` function docs in
  `camera-control-feature-service.hxx` (15/29/35 per inventory bucket F).

## CONTEXT.md moves (service homes only, short prose, English)

- `argus-llm/CONTEXT.md`: `[server]` listener internal-network-only clause.
- `argus-memory/CONTEXT.md`: boot-fails-loudly (Ruling BW), cnats marshal
  constraint, dual event shapes (audit diff vs SocketEmitDto), person/camera
  tombstone vs zone/stream physical delete, `[server]` internal-network clause.
- `argus-notification/CONTEXT.md`: identity-db bounded wait, CORS preflight,
  foreign_keys=OFF reason, audit recipient set.
- `argus-productivity/CONTEXT.md`: same identity-wait/CORS/recipient set +
  SQLite URI-filename-before-first-open clause.
- `argus-camera/CONTEXT.md`: explicit controller registration reason,
  URI-filename clause, aggregation dedupe/dominant-severity/cooldown semantics,
  BlockingTask off-loop note, 9-rule table, matcher dominate/absent semantics,
  detector slot-release + snapshot-overlap concurrency, talk-synthesis
  ordering, identity-db bounded wait.

## Judgment calls / notes for the controller

- Controller ruling honored over the inventory default: NO "Ruling X" tokens
  remain in any code comment (the inventory's condensed-one-liner-with-ID
  suggestion was not used); IDs now live only in the service CONTEXT.md prose.
- One-line `/** */` function docs kept as-is (bucket F); the DTO field
  one-liners in `camera-control/dtos/*` were folded into struct one-liners.
- Inventory entry `argus-camera/src/operator/camera-operator-service.cc:268-269`
  (publishPending cooldown block) was in-function per-statement narrative —
  deleted, the semantics moved to the argus-camera CONTEXT.md budget bullet.
- Inventory said argus-camera had 25 bucket-B lines; I found 2 extra
  in-function comments not listed (`object-bench.cc:43,161`) — deleted too.
- `argus-memory/src/main.cc:56-59` (Ruling BW sqlite3_config ordering) was
  already fully covered by argus-memory/CONTEXT.md; deleted with no prose add.
- No inventory entry was found to be outright wrong in my scope; only the two
  line-number drifts above (extra comments found beyond the map).
- "Fase-2/-3" wording in `camera-config.hxx`/`notification-config.hxx`/
  `productivity-config.hxx` code comments rewritten in English ("phase-2/-3
  defaults"); the "Fase 3" plan references inside the two service CONTEXT.md
  files were left untouched (they name the migration plan's phase).
- `known-person-matcher.hxx` kept its `IKnownPersonMatcher::match` one-liner
  (bucket F) and lost only the `PersonCrop` box field comment (folded into the
  struct one-liner).

Zero behavior change: no declaration, initializer, include, CMake list or
string literal changed; the only code-bearing diff lines are the two lines of
the compile-time-identical constexpr rewrite (`kJetStreamRetentionNs`
declaration + its single use site in argus-camera's `nats-object-event-sink.cc`).
