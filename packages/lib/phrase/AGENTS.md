# argus-phrase

Phrase matching, the phrase catalog and the rule syntax built on them,
plus the static per-language vocabulary they read.

## What this is

A module, not a service. `services/llm`'s memory feature (formation and
recall) and argus-voice (reaction rules) both match phrases and parse rules, so the
cluster lives here rather than in either service.

## Layout

- `src/phrase/phrase-automaton.{cc,hxx}` — the matcher.
- `src/phrase/phrase-catalog.{cc,hxx}` — the catalog that
  loads the vocabulary into the automaton.
- `src/phrase/rule-parser.{cc,hxx}` — the rule syntax.
- `src/phrase/vocabulary.hxx` — `vocabulary`: the four spans over the seed
  data (`spanishPhrases`, `englishPhrases`, `spanishLexicon`,
  `englishLexicon`).
- `src/phrase/lexicon-kind.hxx` — `LexiconKind` (`Predicate = 0`,
  `Kinship`, `FirstPerson`, `Stopword`) with its round-trip pair.
- `src/phrase/memory-type.hxx` — `MemoryType` (`Persona = 0`, `Episodic`,
  `Instruction`, `System`) with its round-trip pair.
- `src/phrase/details/` — the seed data the vocabulary loads:
  `vocabulary-{en,es}.hxx`, `vocabulary-types.hxx`, `phrase-kind.hxx`.

## Rules

- Rule 25: the folder IS the module. One `argus_lib(NAME phrase ...)`.
- Include prefixes are load-bearing.
- Stay domain-neutral. The vocabulary headers deliberately do NOT know
  about extraction: the lexicon built from these seeds lives with the
  extractor that consumes it (`services/llm`'s
  `feature/memory/services/extract/vocabulary-lexicon.hxx`). Re-introducing
  that include would make this module depend on a service.
