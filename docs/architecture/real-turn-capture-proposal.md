# Proposal: opt-in capture of real turns for training (version 2)

Status: proposal, no code. Written 2026-10-07 for the owner's decision. It extends the per-person consent record of
`services/identity` ("Privacy consent", `user_privacy`) and the retention rules of `docs/operations/provisioning-and-models.md`.

## Why

Version 1 of the decider and of the slot extractor trains on public corpora and hand-written rows. Their weakest slice is
real speech (coverage 0.539 on the real variant, against 0.70 to 0.77 on written variants). Only the household's own turns
close that gap, and nothing leaves the machine to get them.

## What is captured

Only for a person who opted in, only on that person's own device and account, only text:

- the transcribed or typed turn, its language, the UTC day (not the second), the role at the time (never the name);
- the decider's answer (tool, confidence, `now`) and what happened next (confirmed, declined, corrected, no answer);
- for a follow-up, the assistant's question that the turn answered.

Never captured: audio, faces, camera frames, voice embeddings, guests and visitors, anyone who is undecided, any turn
spoken by someone other than the account holder (the voiceprint is a soft signal, so an unattributed turn is dropped), any
turn of a minor.

## Where it lives

One table in argus-llm's own database (`database/schema.sql`, additive, mode 0600, under `ARGUS_DATA_DIR`). It never enters
the `/sync` stream, a log line, a backup that leaves the host or a container image. No other service reads it (rule 27); the
owner's review screen reads it through argus-llm's own route.

## Consent and withdrawal

A fifth per-person choice, `turn_capture`, beside `presence`, `face_cameras`, `voice_learning` and `camera_audio`: undecided
means off, the household switch can only turn it off for everyone, and nobody can turn it on for someone else. The notice is
versioned like the others and says, in Spanish first: "Si lo activas, Argus guarda el texto de tus frases en este equipo para
mejorar su comprensión de tu forma de hablar. No guarda audio ni rostros, y no sale de tu casa. Puedes verlas, borrarlas una
por una o todas, y retirar este permiso cuando quieras (Ley N.º 29733: acceso, rectificación, cancelación y oposición)." It
discloses one limit: a model already trained on a turn keeps what it learned until the next retraining, and a withdrawn turn is
excluded from every later one. Withdrawal erases the person's captured turns in the same transaction as the consent write (the
pattern of `eraseForConsent`) and journals `privacy_consent` with the counts, never the text.

## Retention

Unreviewed turns are deleted after 90 days. A turn the person keeps is held until they delete it or withdraw. Nothing is
exported without a review.

## How it enters training

1. The person reviews their own list in the app (keep for training, delete) and supplies or corrects the intent and slots;
   the Owner sees counts and status, never another person's text.
2. An export command, run by the Owner on the host, writes the kept turns to `intent-training/data/real/` (git-ignored, mode
   0600, the same privacy class as `real-turns.jsonl`) in the existing row format with source `real-capture`, license
   `household-private`.
3. The split-by-group pipeline (`scripts/groups.py`) assigns every near-duplicate cluster to one side before anything is
   built: about 30% of the groups go to an eval side that no model trains on (the real-traffic judge grows from 61 turns), the
   rest train with weight 8 as the extractor already does. The sealed-set filter runs over them like any row. A captured turn is
   never written into a sealed set without a separate decision.
4. Retraining is the one documented command of each model card; the manifest records the hash of the captured file and its
   row count, never its content.

## Review and deletion

The person sees, filters and deletes their own turns in the app; deleting one removes it from the table and from any unexported
file the same minute; deleting all or withdrawing also removes it from `data/real/` on the next export run and from every
training set built after that.

## Decisions for the owner

Who labels the intents (the speaker in the app, or the Owner for the Owner's own turns); whether adolescents, who are never
captured under this proposal, may ever opt in with a parent's consent; the 90-day default; whether a short retraining run happens on a schedule or only when he starts it; and whether the notice needs the
review of a lawyer before the first family member other than the owner opts in.
