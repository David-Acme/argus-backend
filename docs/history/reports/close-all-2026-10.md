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
