# Backlog status audit, 2026-10-16 (closing plan, Phase 0)

Every row below was checked against `origin/master` `2e3d48cd` (backend) and frontend
`1d9025e` with `git log -S<symbol>`, `git show <sha>` and `grep -rn` over the worktree.
The backlog items are `realtime-and-backlog-plan.md` §3–§9. During the audit `master`
advanced to `cacb7d10`, one docs-only commit (`docs/plans`); no row here is affected by
it. Statuses: `closed (sha)`, `open`, or `partial (what is missing)`.

## Audit table

| Item | Status | Evidence | Note |
|---|---|---|---|
| F1 Camera | partial | `04fe2534` still-to-save, `61e19b79` + `2a6e28e8` SD day list and timeline, `f761472b` + `127579dd` ONVIF driver with pan/tilt/presets (`PATCH /camera/{1}/preset`, `camera-preset-dto.hxx`), frontend `11b270a` live mute; `2150b906` recording on hold | Closed: snapshot to file, SD timeline, ONVIF control, preset rename/delete, live mute (Android/web; iOS native player audio is Q3's open item). Missing: Argus-side recording (owner holds it; nothing implemented — `record_mode` is storage only), patrol (no hit in `services/camera/src/`), TURN relay (`ice_servers: []` at `services/camera/src/shared/services/stream/go2rtc-manager.cc:235`, no TURN service in `argus-deploy/docker-compose.yml`) |
| F2 Camera conversation | closed (superseded) | `cffc4cc0` adds `camera-watchman-plan.md`, which supersedes F2 | The watchman is the wider goal; no code from F2 |
| F3 Argus calls you | partial | frontend `7a09d2e` incoming call on any screen, `03e45ef` full Telecom call + ongoing notification, `7409cbc` argus-push FCM Nitro module; backend `1f957a55` durable ring/cancel/alert outbox rows, `c4869bc2` tunnel sealed-push consumer | Closed: Android ring-to-hang-up, FCM push path, relay/tunnel delivery. Missing: iOS CallKit + PushKit (no `CallKit`/`PushKit` anywhere in the frontend), APNs VoIP, and the TURN relay of an away-from-home call (see F1) |
| F4 Guard duress and PIN | partial | `36bf9b14` duress phrase, `751cc4d8` PIN lockout survives a restart | Missing: the single-member household case, an open design fork left to the owner (`services/guard/CONTEXT.md:1253`) |
| F5 Siren button | open | decided 2026-10-16 (`f2ab48e9`, closing plan Phase 4b) | No `app_siren_enabled` in code; the panic button's siren icon is unrelated. Unit queued |
| F6 Productivity | partial | backend `5b492ae7` snooze + recurring reminders, `50d8ffb3` assignee set/clear/leaves, `96a92f4c` `calendar.list_events` shared, `d6d325db` repeat-rule parse; frontend `f4957a6`, `7ec3b21`, `789523b`, `620fa62`, `a7ddb36`, `d946fc3` | Closed: snooze, recurrence, share screens, assignee, notification delete, summary count, agenda zone-offset cache (F6c). Missing: F6a assistant-created recurring reminder (no recurrence in the turn slots; waits for the Laya cutover) and F6b one-occurrence edit (out of scope until the series model settles) |
| F7 Agronomy module | open | `services/settings/modules.json:393` still `kind: coming_soon` | Nothing built |
| F8 Messenger | open | designed in `cbd49b04` (`parallel-conversations-plan.md`); no relay tool on master | Phase 6 |
| F9 Parallel conversations | open | same plan, `cbd49b04`; no per-user conversation store on master | Phase 6 |
| S1 mTLS internal | partial | `e3641af1` internal CA, `096d280b` `internal-tls` + `tls:` targets, `3211c1c6` `tls-health`, `6144bf1d`/`2e989bf1` services and callers dial `tls:` | Missing: the cleanup that removes the plaintext internal listeners, after the owner's deploy check (plan §S1 order) |
| S2 Tunnel home link | closed (superseded) | Ruling of 2026-10-12 in the plan (§5, after the S list); relay route landed frontend `89a6d60`, `a438e73` | Superseded by the Argus Cloud project (its C2) |
| S3 Per-service NATS | partial | `7b5bec2d` permission table, `scripts/provision-host.sh:295` issues one nkey per service, `05a649b0` the gate `nats-users-check.sh` | Missing: the shared `argus` user is still emitted (`scripts/lib/nats-users.sh:189`); the owner's deploy step and its removal remain |
| S4 Per-service S3 | partial | `ba929bd9` no key on a command line, `rustfs-init` per-writer accounts, `[storage.s3] credentials_file` in `S3StorageService` | Missing: same owner deploy step; configs still carry the shared `access_key`/`secret_key` (`services/identity/config.toml.example:130`) |
| S5 Legacy refresh tokens | open (accepted) | `services/auth/CONTEXT.md:442-451` | A legacy token two rotations old is refused without revoking its family; accepted, expires with the pre-`sid` window |
| Q1 Frontend tests | partial | frontend `26409a3`, `7b90650`, `a77120e`, `9e5255c`; `tests/unit/` mirrors `src/` (features, shared, core, app) | Closed: e2e suite (tester-army runner, `CONTEXT.md:4175`), two-device cases, critical screens, visual regression at three widths. Missing: explicit component tests where the e2e covers welcome/response/module dialogs (`tests/unit/features/{auth,response,modules}` cover models and hooks) |
| Q2 Welcome hero a11y | closed | frontend `3fe659a` (`WelcomeHero` one labeled button, `welcome-screen.tsx:121`) | Decoration out of the tree; the unlabeled replay is gone |
| Q3 Device validation | partial | frontend `7297f6c` pairing keyboard, `11b270a` live mute, `bc0b0df`+`52f148a` permissions; `CONTEXT.md:4295`, `:4310`, `:4324` list the open device items | Missing: device runs of camera live audio and WebRTC on phones, the iOS build of the camera media module, and audio on the iOS native player |
| D1 clang-tidy paydown | open (ongoing) | `scripts/lib/tidy-baseline.txt` now 2,308 findings over 1,313 TUs (plan: 2,358 over 1,173); paid down in `c3c8ea4c`, `97249df0` | The ratchet holds; paydown per unit |
| D2 Quality debt | partial | `8e8c1101` sync drain for the audit sweep (`services/sync/src/app/main.cc:478`) | Missing: `[[nodiscard]]` on `Syncable` (`packages/contracts/sync/src/sync/syncable.hxx:21-31` has none), an `AuditRetentionService` timer test (none under `services/sync/tests/unit/`), the Drogon exit-time heap corruption (`docs/history/plans/architecture-plan.md:983`; auth suites still carry their own `main()`) |
| D3 Guard retention | partial | code clamps 1–60 default 30 (`services/guard/src/config/guard-config.cc:276`, `guard-feature-service.cc:1002`); prose aligned (`services/guard/CONTEXT.md:1502`) | Missing: the settings table row still reads 1-3650 / 90 (`services/guard/CONTEXT.md:578`) |
| D4 voice and tunnel layouts | open | `services/voice/src/` has no `app/` (config, feature, main.cc, shared, test-support); `services/tunnel/src/` keeps client/core/link/net/protocol/relay/server | voice is the named gap; tunnel exempt by D19 but the row stands |
| D5 Confirm and close | closed | 3d step 5: frontend `fd31bd0` sets `src-tauri/src/net/discover.rs:8` to `_argus-route._tcp.local.`; `scripts/lib/check-deps.py:153` resolves `argus::` aliases and tiers; `src/features/visitors/` + `b6874ca`; `services/camera/src/feature/operator/known-person-matcher.hxx:38` | All four confirmed done |
| D6 Stale docs | partial | `5fec07db` fixed `AGENTS.md` and `services/sync/CONTEXT.md` | Missing: `tool-calling-reduction-plan.md` carries no superseded marker (untouched since `d182c5eb`); frontend `CONTEXT.md:305` still lists chat/workspace/settings as pending and `:801` still says WatermelonDB sync pending |
| D7 guard_encounter_outbox | open | still read for the one-time copy (`services/guard/src/feature/guard/guard-schema.cc:538-547`) and pinned by `guard-migration-test.cc:520` | Needs an owner reset or a later release |
| D8 Writerless tables | open | server still serves them (`services/sync/src/feature/transport/services/synchronized-service.cc:360-363`, `:171`, `:433`; `synchronized-dto.hxx:133,136`) | Correct until the supported client set moves off v10/v11 schemas |
| D9 tts close | closed | `2e41b5ae` | Verified on master |
| D10 build-all self-test | closed | `a62a7f78` | Verified on master |
| D11 runner race | closed as measured | `797b70c8`; classification in `realtime-and-backlog-plan.md:441-446` | No change beyond the two fixtures |
| D12 tts unary admission | open | `services/tts/src/feature/synthesis/controllers/tts-controller.cc:97-119` takes no `streams_` slot | The stream route is the only admission; no 429 on the unary path |
| D13 tts tests on a missing model | open | `REQUIRE(TtsService::instance().isLoaded())` at `services/tts/tests/unit/tts-wire-test.cc:274` and `tts-rpc-test.cc:329` | Should skip with a reason; `docs/operations/build-and-test.md` links the models meanwhile |
| D14 Thread read marking | open | `services/notification/src/feature/notification/controllers/notification-controller.hxx:16-32` has read/ack/delivery-summary, delete by id and by thread | No server-side `PATCH /notification/thread/{threadKey}/read` |
| §8.1 Laya + GLiNER + utf8proc | partial | utf8proc `2.12.0` (`conanfile.txt:14`, `text_norm::nfc` at `services/llm/src/feature/llm/controllers/llm-controller.cc:226`); deciders exist (`services/llm/src/app/main.cc:208` laya, `:238` gliner) | Not flipped: `[decide] engine = "router"` and `[extract] engine = "nuextract"` (`services/llm/config.toml.example:45,126`); fastText and NuExtract still on master. Phase 5 item 1 |
| §8.2 Siren and consent | open | decided 2026-10-16 (`f2ab48e9`) | Same unit as F5 |
| §8.3 Camera conversation variant A | closed (superseded) | `cffc4cc0`, `camera-watchman-plan.md:4` | Same successor as F2 |
| §8.4 Settings profiles | partial | `services/settings/profiles.json:59-60` already reads `minRamGb` 14 / 7, and balanced keeps `tts.pocket_variant_es = "fast"` (`:20`) | Missing: `gpu_layers = 0` written explicitly (`profiles.json:48,51` set 999; `services/llm/config.toml.example:39,129` set -1). Phase 4b unit |
| §8.5 Remote access | partial | decided 2026-10-16 (`f2ab48e9`); LAN-first probe and offline notice landed frontend `89a6d60`, `a438e73`, `c8c5438` | Missing: the relay-down acceptance (two-device test with the relay stopped); no write-refusal copy found in the frontend, the offline toast only shows cached data (`src/core/i18n/locales/es/common/index.ts:21-24`) |
| §8.6 VLM gRPC cutover | partial | VLM serves a mutual TLS gRPC listener (`1eeb8d66`) and guard's caller accepts a target (`services/guard/src/app/main.cc:309-312`) | Missing: the cutover itself — guard's example still dials `vlm_url = "http://127.0.0.1:7031"` (`services/guard/config.toml.example:147`) and the report listing the frozen HTTP wire. Phase 4b unit |
| §8.7 Migration-plan opens | closed | orphan tables dropped (`0cfb0110`; no `voice_session`/`voice_message`/`context_note` on master); Artifactory and QuickJS occur only in the plan documents, never in the build or code (`grep -rn` outside `docs/history` is empty) | Resolved as dropped, local Conan, manifest-only |

### Phase 5 gate (§10 order, read off the table above)

1. §3 Laya and GLiNER in the product — open (§8.1: engines behind config, defaults not flipped).
2. F1 Argus-side recording — open (owner hold; patrol and TURN also open).
3. F4 guard duress and PIN lockout — open (phrase and lockout landed; single-member case open).
4. F6 productivity remainders — open (F6a, F6b).
5. The open S items — S1 cleanup, S3/S4 owner deploy steps, S5 accepted gap.
6. Q1–Q3 — Q1 near closed, Q2 closed, Q3 device validation open.
7. The D items — D1, D2 (three of four), D3 (table row), D4, D6 (two of four), D7, D8, D12, D13, D14 open.

## REVIEWER-RESUME queue status

Batch 3 (accepted, merged): all landed on `master` under new hashes —
`git cherry origin/master fix/call-end-cancel-push` answers `- ad3f13d4` (master
`0aef16cd`), `- d7d124fd` for the tunnel log (`c73c658a`), the three scripts commits as
`17b67adb`, `f5e71b94`, `74149e6a` (identical subjects), identity `a2f37b64`, and
the call-push log `1e91ff8c` as `7140e548`. The
cloud commits (`5302cea`, `ed83167`) are the separate `argus-cloud` repo, not audited
here.

In review (`fix/alert-intent-contract`, `fix/alert-silence-with-ring`): landed.
Server: `1ec5de5c`→`b78f0bc0`, `d0e0eb5e`→`493a3528`, `4236e135`→`0dd6beba`,
`ed1e24dc`→`c7874b90`, `25e70d06`→`6585f019`, `72efcb1f`→`60585ff3`,
`4b853813`→`9e3cd963`, plus the shared service `3774645b` and the RPC tests
`2aff526f` (`notification-rpc-test.cc:604`). Client: all nine commits of
`fix/alert-silence-with-ring` carry identical subjects on frontend `master`
(`a02ef39`→`980e691`, `b041a9a`→`6850f3e`, `079c53c`→`96f8784`, `b017963`→`df900ec`,
`7ea4db4`→`d312419`, `c499012`→`9fe140c`, `a40a399`→`6bed33b`, `0810c77`→`b8c0cba`,
`7357d63`→`e37dd5b`). `884cc0b7` was partial and never compiled alone; it is
superseded by `3774645b`.

Queue after that:

1. CloseCallRoom RPC — landed, `6f7610fe` (sync closes the room on notification's
   request), with `ca8b65a7` admitting the notification caller to Farewell.
2. Accounting — landed: `147474bb` (a claimed call stays completed), `901a78f1`
   (stale-call sweep closes only empty rooms), expiry at `call-engine.cc:423`, `:686`.
3. Missed-call copy — landed, frontend `df22c73`.
4. Foreground unit — landed: `2c562f4` (in-app banner) with `ef17f78` (listeners from
   the root layout); the native dedupe by call id is in `CallActivity.kt`.
5. End-of-call UX — landed: `d44395c` ("Llamada finalizada" 2 s, rejected ring closes),
   `db83465` (native ringing screen matches the in-app surface), `d3ef704`, `0b76af8`.
6. Offline sweep — mostly landed: permissions onboarding `bc0b0df` + `52f148a` +
   `7ce94bd`, pairing keyboard `7297f6c`, 409 mapping
   (`src/features/auth/screens/pairing-screen.tsx:33`), barge-in backend `4141e8a2`
   with the near-field gate `1e6b86a9`. Not found: a welcome gate for Android 9
   (welcome-screen.tsx honours reduce-motion only).

Device batch: not run (all nine pending; S21 unplugged). The batch-3 merge has
happened (evidence above); the final build, boot proof and golden verify that preceded
it leave no trace in the tree, so they are taken from the resume, not verified here.
Implementer state: `2211ca0a` and `2587b6d7` live on `fix/alert-intent-contract` only;
the SIGSEGV they were debugging is closed by the landed RPC tests (`2aff526f`).

## Housekeeping: branches landed (delete after review)

Backend, verdict from `git cherry -v origin/master <b>` (a `-` line is
patch-equivalent) and `git log --oneline origin/master..<b>`:

- `fix/alert-intent-contract` — 16 commits, 15 patch-equivalent; the 16th
  `0b9c3c92` landed as `6f7610fe` with the CONTEXT note and a test delta.
- `fix/call-end-cancel-push`, `fix/call-push-delivery-log`,
  `fix/tunnel-delivery-hex-log`, `track/c-tunnel-rtc`, `track/call-durable-note`,
  `track/d-service-certs`, `track/d-skips`, `track/d-sni-dispatch`,
  `track/voice-gate-b3`, `track/b3-guard-flake`, `track/b3-warnings`,
  `track/a-push-vectors`, `track/c-camera-commit1-rebased`, `c0b-stick`,
  `c1-parked`, `mc-a`, `mc-ab`, `mc-b`, `mc-base` — every commit
  patch-equivalent (plus=0).
- Fully contained in `master` by ancestry (`git log origin/master..<b>` empty):
  `b1-mold`, `b3-ccache-b`, `b6-module-scan`, `c0b`, `c1c`,
  `claude/argus-backend-audit-l5zqp4`, `codex/f8-c4-final`, `e2e-turn`,
  `f10-hygiene`, `integ/backend-candidate`,
  `migration/fase-1-gateway`, `migration/phase-0`, `poolenv-env-override`, `r1`,
  `r1-release`, `r1b-release`, `track/a-f6`, `track/a-tidy`, `track/b4-bridge-log`,
  `track/c-camera-commit1-on-4c6c47be`, `track/call-backend`, `track/call-durable`,
  `track/d-naming-vectors`, `track/d-tunnel-close`, `unit-bilingual-c1`,
  `unit-floor`, `unit-pool`, `unit-tool-lang`.

Frontend:

- `fix/alert-silence-with-ring` — every commit has its identical subject on
  `master` under a new hash (mapping above), though patch ids differ (the batch
  reworked them).
- `fix/telecom-voip-audio`, `track/call-android`, `track/call-js` — every commit
  patch-equivalent.
- Fully contained by ancestry: `cand/batch3-frontend`,
  `fix/call-cancel-dismisses-ring`, `fix/call-hangup-stops-voice`,
  `fix/login-proof-native-crypto`, `fix/missed-call-public-version`,
  `integ/frontend-candidate`, `track/a-f6-ui`, `track/c-f1-mute`.

In use by the closing-plan batch 2026-10-16: `fix/phase0-backlog-audit`
(`wt/backend-phase0`, this audit, one commit ahead of `master`),
`fix/call-opening-reason`, `fix/guard-vision-prompt`, `fix/llm-tier-memory`,
`fix/threadbudget-interactive` and frontend `fix/session-recovery-backup`. None of
their units has landed yet, so the ancestry verdict is vacuous (the unit is in
flight, not landed); they are not cleanup candidates.

Not landed, keep: backend `fix/identity-pairing-refusal-log` (10 divergent),
`fix/native-stack-keep-runtime-state` (10), `fix/pki-openssl3` (1),
`track/b-infra` (13), `track/b-tls-clients` (17), `track/voice-gate` (37),
`track/b4-conversations`, `track/b4-messenger`, `track/b4-watchman`,
`track/b4-guard-mode-voice-confirm`, `track/b4-turn-home`, `track/b4-stt-quality`,
`track/b5-watch-phone`, `track/c-guard-camera` (7), `track/camera-onvif-c225` (20),
`track/d-remote-auth`, `design/push-calls`, `scratch/a-ci-rewrite`,
`wip/track-b-phase1-20261014T061220`, and the older `c1-held`, `c1b-parked`,
`c1c-nofix`, `mc-ab-id`, `r1b-int-*`, `b2-split-dwarf`, `b3-ccache`;
frontend `track/call-desktop` (1 divergent), `track/d-local-first` (7),
`track/frontend-offline-toast` (5), `wip/d-calm-status`,
`fix/native-ringer-follows-mode` (1 of 2 divergent).

## Housekeeping: worktrees

Backend, from `git worktree list` with dirty count `git -C <path> status --porcelain | wc -l`:

| Worktree | Branch | Dirty |
|---|---|---|
| `/home/acme/Desktop/argus/backend` | master | 0 |
| `.claude/worktrees/agent-aad931bf8ed71cf52` | worktree-agent-aad931bf8ed71cf52 | 0 |
| `wt/backend-a` | track/a-ci | 0 |
| `wt/backend-a2` | track/a-push-vectors | 0 |
| `wt/backend-b` | track/b-infra | 0 |
| `wt/backend-b2` | track/b-tls-clients | 0 |
| `wt/backend-b3-base-bench` | detached | 0 |
| `wt/backend-b3-durable` | track/call-durable-note | 0 |
| `wt/backend-b3-impl` | track/call-durable | 0 |
| `wt/backend-b3-warn` | track/b3-warnings | 0 |
| `wt/backend-b4-bridge-log` | track/b4-bridge-log | 0 |
| `wt/backend-b4-conv` | track/b4-conversations | 0 |
| `wt/backend-b4-msg` | track/b4-messenger | 0 |
| `wt/backend-b4-mvp` | track/b4-guard-mode-voice-confirm | 0 |
| `wt/backend-b4-stt` | track/b4-stt-quality | 0 |
| `wt/backend-b4-turn-home` | track/b4-turn-home | 0 |
| `wt/backend-b4-watch` | track/b4-watchman | 0 |
| `wt/backend-b5-watch-phone` | track/b5-watch-phone | 0 |
| `wt/backend-c` | track/c-guard-camera | 0 |
| `wt/backend-c2` | track/c-tunnel-rtc | 0 |
| `wt/backend-ca416` | detached | 0 |
| `wt/backend-call` | track/call-backend | 0 |
| `wt/backend-call-opening` | fix/call-opening-reason | 0 |
| `wt/backend-cores` | fix/threadbudget-interactive | 0 |
| `wt/backend-d` | detached | 2 (submodule pointers ncnn, sherpa-onnx) |
| `wt/backend-d2` | track/d-skips | 0 |
| `wt/backend-design` | design/push-calls | 0 |
| `wt/backend-guard-flake` | track/b3-guard-flake | 0 |
| `wt/backend-guard-prompt` | fix/guard-vision-prompt | 0 |
| `wt/backend-integ` | integ/backend-candidate | 0 |
| `wt/backend-llm-tier` | fix/llm-tier-memory | 0 |
| `wt/backend-onvif` | track/camera-onvif-c225 | 0 |
| `wt/backend-phase0` | fix/phase0-backlog-audit | 0 (this audit) |
| `wt/backend-pki` | fix/pki-openssl3 | 0 |
| `wt/backend-voice-gate` | track/voice-gate | 7 (unlanded voice-gate edits) |
| `wt/backend-voice-gate-b3` | track/voice-gate-b3 | 0 |
| `wt/call-end-cancel` | fix/call-end-cancel-push | 0 |
| `wt/ci-rewrite-a` | scratch/a-ci-rewrite | 0 |
| `wt/native-stack-state` | fix/native-stack-keep-runtime-state | 0 |
| `wt/notification-push-log` | fix/alert-intent-contract | 0 |
| `wt/tunnel-delivery-log` | fix/tunnel-delivery-hex-log | 0 |
| `/home/acme/argus-final` | detached | 0 |
| `/home/acme/argus-gate` | cand/batch2 | 0 |
| `/home/acme/argus-squash` | final/master | 0 |
| `/home/acme/ci-squash` | squash/ci | 0 |
| `~/.cache/argus-scratch-backup/backend13/{floor,pool,poolenv,unit-bilingual-c1}/worktree` | unit-floor, unit-pool, poolenv-env-override, unit-bilingual-c1 | 0 |
| `/tmp/claude-1000/.../scratchpad/mvp-merge` | track/b4-mvp-models | 4 (unlanded mvp-bundles edits; tmpfs) |

Never delete without review: the three dirty rows above. Stale and clean, best
candidates to remove with their landed branches: `call-end-cancel`,
`tunnel-delivery-log`, `notification-push-log`, `backend-c2`, `backend-a2`,
`backend-d2`, `backend-b3-warn`, `backend-b3-durable`, `backend-voice-gate-b3`,
and the four `~/.cache/.../backend13/` worktrees.

Frontend, from `git -C /home/acme/Desktop/argus/frontend worktree list`:

| Worktree | Branch | Dirty |
|---|---|---|
| `/home/acme/Desktop/argus/frontend` | master | 0 |
| `wt/alert-silence` | fix/alert-silence-with-ring | 0 |
| `wt/frontend-a` | track/a-f6-ui | 0 |
| `wt/frontend-batch3` | cand/batch3-frontend | 0 |
| `wt/frontend-c` | track/c-f1-mute | 0 |
| `wt/frontend-call-android` | track/call-android | 0 |
| `wt/frontend-call-desktop` | track/call-desktop | 0 |
| `wt/frontend-call-js` | track/call-js | 0 |
| `wt/frontend-d` | track/d-local-first | 0 |
| `wt/frontend-integ` | integ/frontend-candidate | 0 |
| `wt/frontend-proof` | fix/call-cancel-dismisses-ring | 0 |
| `wt/frontend-session-recovery` | fix/session-recovery-backup | 0 |
| `wt/frontend-telecom-voip` | fix/telecom-voip-audio | 0 |
| `wt/ringer-apk` | detached | 0 |
| `wt/ringer-mode` | fix/native-ringer-follows-mode | 0 |
| `~/frontend-squash` | squash/d | 0 |
| `/tmp/claude-1000/.../scratchpad/frontend-toast` | track/frontend-offline-toast | 1 (tmpfs) |

`wt/frontend-telecom-voip` is the named example: its branch is fully landed, so the
worktree is stale and clean. Never delete `/tmp/.../frontend-toast` without review.
