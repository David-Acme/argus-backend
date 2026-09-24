# argus_contracts_voice

The voice boundary's vocabulary: the language a session, a reaction rule and a
stored user profile each name.

## What this is

A CONTRACT, and the smallest of the ten: one header, one enum, no
dependencies. The include root is `src/`, so a consumer writes
`<voice/voice-lang.hxx>` and links `argus::contracts::voice`. Two CMakeLists do
— `services/identity`, because a user profile stores the language, and
`services/voice`, which owns the boundary and reads it per session.

## Layout

- `src/voice/voice-lang.hxx` — `VoiceLang` (`System = 0`, `Es`, `En`) with
  `voiceLangToString`/`FromString` over the three spellings (`""`, `"es"`,
  `"en"`). 3 files include it.

## Rules

- The empty string is a value, not a missing one: `System` means "follow the
  device", and the database stores the string code, so `""` is a legitimate
  stored row. Every unknown spelling falls back to it rather than throwing.
- The enum is `uint8_t`-backed but the ordinals are never stored and never
  sent: the string spellings are the contract, and adding a language means
  adding a code, not renumbering.
- The wire schema is `argus/voice/v1/voice.proto` under
  `packages/contracts/proto/`, compiled by `packages/clients/voice`, and a
  request carries these spellings in its `language` field. This package is the
  C++ half of that same vocabulary.
- Rule 25: the folder IS the module. One `argus_contracts(NAME voice ...)` with
  an explicit source list, never `file(GLOB)`.

## Tests

- `tests/unit/voice-contract-vocabulary-test.cc` — the three spellings'
  round-trip, including `""` for `System`, and the fallback.
