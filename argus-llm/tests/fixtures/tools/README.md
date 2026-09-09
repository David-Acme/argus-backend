# Tool-calling evaluation set

Labelled utterances, rescued from `labs/intent-data/` when `labs/` was deleted
(f8-a2). They were fastText training data; with the classifier retired they are
the accuracy harness for the LLM's own tool calling.

- `check.tsv` — the evaluation set: 103 cases, `label<TAB>utterance`
  (56 `camera`, 20 `memory_save`, 27 `none`). This is the file the 2026-08-11
  gate scored when it reported "0/20 memory_save, 2/56 camera false positives"
  against `LFM2.5-1.2B-Instruct-Q4_K_M.gguf`. The active model is newer, which
  is why f8-b1 re-measures before anything is wired.
- `negatives.tsv` — utterances that must fire nothing.
- `train.tsv`, `valid.tsv`, `test.tsv`, `test2.tsv` — the old supervised splits,
  kept as extra labelled material.
- `usage.tsv`, `usage-curated.tsv` — utterances logged from real lab sessions.

Spanish and English mixed, matching the assistant's own languages.
