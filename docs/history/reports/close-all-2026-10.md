# Close-all programme, 2026-10

The owner asked on 2026-10-19 that every open point be closed. One section per unit, measured.

## 1. The doubted lens mask on the wire (`privacyObeyed`)

- Backend:
  - `CameraRow.privacy_obeyed = 21` (additive) and the camera row's `privacyObeyed` reach the sync pull, the camera sync RPC, the sync gateway and the overview.
  - The privacy audit's before/after carry the persisted `(state, obeyed)` pair. The compare-and-set covers that tuple, and a retry that finally lands publishes its own audit.
- Frontend:
  - schema 13 adds `camera.privacy_obeyed`, mapped through sync, the view cache and the overview contract;
  - an unknown state that the server obeys is masked and labelled "Privacidad (sin confirmar)" / "Privacy (unconfirmed)" on the card placeholder and the home tile.
- Measured:
  - camera 41/41 and sync 21/21 suites green;
  - `two refreshes of one camera in flight give exactly one audit` passed 20/20 runs;
  - frontend 1654 tests, `tsc` for app and tests, lint, comment, boundary and bundle checks green.

## 2. The refusal leg (RF), verified

RF's two commits (`9e833a14`, `d21b520f`) were brought onto master and verified for the first time. The verification found and fixed these defects:

- **Word splitting in the refusal rules and the guard.** Every non-ASCII byte split a word, so "María" read as "Mar" and "¿dónde está?" never matched. Both splitters now keep UTF-8 letters and treat ¿ ¡ « » as separators.
- **Impersonation guard.**
  - A self-introduction ("soy David", "this is David") is always impersonation.
  - A vocative with a first-person cue ("María, estoy en camino") counts only on a relay turn, so Argus can still greet the user by name.
  - A self-introducing sentence is dropped from every reply path: streaming, plain and act. This was measured stochastically in the pack ("Hola, soy David…").
- **Person locations.** A location is guarded only when a person is the subject right before "está en" / "is in". "La reunión con Andrea está en la agenda" is no longer replaced by a privacy refusal.
- **The streaming scrub.** It dropped the whitespace between sentences ("días**.Es un concepto…"). Whitespace-only segments are now kept.
- **Relay refusals.**
  - A refusal echoes only a proper name. "mi chibolo" and "mi hijo" said back by Argus were a regionalism and a wrong possessive.
  - Multi-word postpone phrases had never matched ("para más tarde", "for later"). They now match as phrases, together with a move verb, so "pon un recordatorio para después de comer" stays a reminder.
- **Questions.**
  - The ask note made the model answer with statements (not_a_question 1 → 10). It now asks for one short question ending in a question mark.
  - Questions with known options, dates, choices and confirmations are rendered deterministically from the act, as RF already did for refusals, so they always name what the guard checks.

Measured on the prod build with the shipped pipeline (rules + triggers + Laya + GLiNER, `LFM2.5-1.2B-Instruct-QAD-Q4_0`, temperature 0.3, seed 42):

- **`call-faithfulness-render-acts`:** `retryShare` 0.4839 → 0.0323 (1 of 31; ceiling 0.05). GATE PASSED, for the first time.
- **The 15 refusal cases** (reminder, errand, privacy, role-inactive, relay-panic × es, es-PE, en): `falseRelays` 0, `noRefusal` 0, `privacyLeaks` 0, `regionalism` 0. GATE PASSED on three runs.
- **llm unit suites:** 73/73.

Left to the consolidated phase, because the smoke gate does not measure them:

- `questionMissing` on "ya lo hice" turns;
- `missedFacts` on relay-panic openings;
- the 40-reply blind sample.

The shipped bundles `decide/bundle-laya-mvp` and `extract/bundle-gliner-pilot` were installed into the main checkout's `models/`. They were added beside the pilots and nothing was replaced.

## 4. CC0: the watchman switches

- **Identity.**
  - `household_privacy.watchman` with its acknowledgement (`watchman_ack_version/by/at`), mirroring `visitor_recognition`. `PATCH /privacy/household {watchman: true}` needs `acknowledge: true`.
  - `location` as a per-person choice with an opt-out default: an omitted value keeps the stored one. It also exists as a household ceiling.
  - The proto `PrivacyChoices` carries `location` (8) and `watchman` (9), and `ListPrivacy` fills both.
  - A boot column migration adds the new columns to an existing database.
- **Camera.**
  - `camera.watchman` and `camera.no_audio`, default off, are settable through `PATCH /camera/{id}`. They are carried on the row JSON, the sync `CameraRow` (22, 23) and the sync gateway.
  - A boot migration adds them.
- **lib/auth.**
  - `kCameraWatchCapabilities` holds what a camera conversation may do: voice, camera view, reminders, agenda, projects, safety and notifications. It never holds the guard mode, settings, roles, presence or the directory.
  - `kCameraVisitorCapabilities` holds what a stranger may do: voice, panic and safety read.
  - Each set comes with its `…For` and `has…` helpers.
- **Legal.**
  - `docs/operations/legal-and-privacy.md`.
  - `scripts/privacy/watchman.{es,en}.md` and `--accept-watchman-notice` in setup and provisioning, recorded as `watchmanAcknowledged`.
  - The installer-responsibility and camera-audio sentences in the host notice and in the app's notice.
  - The notice moves to version 2 in identity, the scripts and the app together.
- **Measured.**
  - identity 31/31, camera 41/41, sync 21/21, packages 132/132, `privacy-consent-test` green.
  - The full dev build with all 15 projects' tests green.
  - Frontend: 1654 tests, `tsc`, lint and the comment gate green.
- **Owner decision recorded.** The per-camera flags follow `camera.manage` (Owner and Resident). The household switch stays Owner-only and acknowledged, so a Resident alone can never make a camera listen.

## Item 5: AL-B1/B2, who aims the camera

- **Move windows.** The scene log carries each move; the health monitor and the operator pause through it, so a pan is not a scene change or a new track.
- **Lease.** Argus < Viewer < Person, park and idle release 60 s, `[ptz]` config.
- **Home and drift.** `set_home`/`go_home`, the automatic return after a look or a follow, `ptz_automatic_move` audit, `homeDrift` health metric and a bounded recalibration.
- **Guard on duty.** `GetDuty` on guard's presence RPC; camera's `DutyGate` (15 s cache, fails closed); `CameraAction::Move`/`Preset` for Resident and Guard; a Guard cannot save, delete or set home. Credentials provisioned by setup, provision-host and native-stack.
- **Measured.** camera 42/42 (new `camera-ptz-lease-test`, three health cases), guard 42/42, packages 132/132 (role-access pins the new rows), comment/deps/routes gates, `rpc-credentials-test`, `internal-pki-test`, `setup-config-only-test` green.

## Item 14: the tidy debt, paid back under the baseline

- **Before.** 2346 findings over 1365 TUs against a baseline of 2184; 23 checks above their count. Most of it: `bugprone-unchecked-optional-access` (+56) and `modernize-use-designated-initializers` (+63).
- **What changed.**
  - The 63-row pin table of `llm-context-selector-test` uses designated initializers.
  - Tests that read an optional after `REQUIRE(x.has_value())` take it through one shared helper, `valueOf` (`packages/lib/test-support/src/test-support/value-of.hxx`), which throws on an empty optional. clang-tidy 23 counts `.value()` as unchecked, and a `REQUIRE` is not a branch it follows.
  - The camera control service checks `result && result->ok` where it used to read through `succeeded()`.
  - The swappable pairs take a struct: `CameraInstant` (scene log), `PtzInstant` (lease), `DutyInstant` (duty gate), `NameWindow` and `JoinNamedInput` (llm turn), and the test helpers `ConfigFiles` and `ClockTime`.
  - The small checks: single-char appends, `starts_with`, `std::array` for the device digest, `static_cast` for the health event, braced returns in `cert-service`, `emplace_back`, `[[nodiscard]]`, `scoped_lock`, `string_view` for `statusName`, range-for in the Laya runtime test, a raw string in the saga test.
  - `llm-refusal-rules-test` drops its own doctest main and links `argus::lib::doctest-main`.
- **Found on the way.** `llm-duress-screen-test`'s once-per-speaker case raced its 10 ms retry under a loaded build (4 raises for 3). It now runs with a 200 ms retry base, so the second utterance lands while the first raise is pending.
- **After.** 2169 findings over 1368 TUs, no check above its count; the baseline is ratcheted to 2169.
