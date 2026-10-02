# argus-text

The text and payload utilities: normalisation, the JSON helpers (parse,
serialize, flat object diffs) and the hashing and encoding the fleet shares
(SHA-256, FNV-1a, base64).

## What this is

A PACKAGE, not a service: no I/O, no state, no config, no `main`. Eleven
units outside `packages/lib` link it — the `sync` and `vlm` clients,
`contracts/sync` and eight services (auth, camera, guard, identity, llm,
notification, productivity, sync) — which is what a utility package looks like
when it is the fleet's only one: what is here is
here because more than one owner needs it, not because it is generic.

It absorbed the `json` and `hash` packages; `json-diff.cc`, `base64.cc` and
`text-norm.cc` are real translation units, so it is NOT the header-only
package section 2.3 lists it as — that line described the tree before the
absorption.

## Layout

- `src/text/text-norm.{cc,hxx}` — `text_norm`: UTF-8-aware word splitting
  (accented words stay whole), word sets, whitespace collapsing. The one
  splitter the matchers share.
- `src/text/json-util.hxx` — `json_util`: `toString` (compact, null-safe),
  `fromString` and `isValid` over jsoncpp.
- `src/text/json-diff.{cc,hxx}` — `JsonDiff` with `Change`/`ChangesDiff` and
  `ChangesComparisonResult`: `createFlatDiff`, `compareChanges`,
  `compareObjects`, `applyChanges`, `fromJsonString`, `toJson` — the flat
  object diff the sync stream and the audit trail carry.
- `src/text/sha256.hxx` — `argus::hash::Sha256`, self-contained so no service
  needs a crypto dependency to hash a payload.
- `src/text/fnv-hash.hxx` — `Fnv1a`, the content checksum the migration tools
  compare.
- `src/text/base64.{cc,hxx}` — `base64::encode`/`decode`, the latter
  answering `std::optional` so a malformed input is not an exception.

## Rules

- Rule 25: the folder IS the module. One `argus_lib(NAME text ...)`
  declaration, explicit source lists, never `file(GLOB)`. The include root is
  `src/`, so consumers write `<text/json-util.hxx>`.
- Everything here is a pure function over its arguments: no config read, no
  log line, no global, no thread. A utility that needs any of those belongs to
  the domain that owns it, not to this package.
- Nothing here includes Drogon, in a header or in a source: the JSON type
  these helpers traffic in is jsoncpp's. Drogon is on the link line because
  the helper passes every `DEPENDS` entry PUBLIC and jsoncpp's include
  directories arrive with it.
- Hashes are pinned by known-answer vectors, never by a round trip against
  themselves: `sha256-test` asserts the standard vectors and the 56/64-byte
  block boundaries, because a hash that agrees with itself can still be wrong.

## Tests

`tests/unit/sha256-test.cc` — the known-answer vectors, the block boundaries,
chunked updates against a single update, and the length-prefixed field
encode.

`tests/unit/json-diff-test.cc` — the wire form of a diff: every entry carries
both `previous` and `current`, null included, because a replica applies a
change only when the `current` key is present and a field cleared to null
must reach it.
