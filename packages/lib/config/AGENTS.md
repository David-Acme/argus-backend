# argus-config

The boot helpers: the TOML reader every binary resolves its keys through, and
the `IService` interface the optional services implement.

## What this is

A PACKAGE, not a service: no route, no listener, no database, and no refusal
of its own — which is why it depends on neither `argus::lib::http` nor
`argus::lib::errors`. The health controller and the listener resolution that
used to live here moved to `argus::lib::http`; what is left is boot-time
configuration and nothing else.

Its reach is the widest of any lib — seventeen units outside `packages/lib`
read their keys from it: the `llm`, `stt`, `tts` and `vlm` clients and all
thirteen services — so the resolution order below is a fleet-wide contract,
not a local choice.

## Layout

- `src/config/config-service.hxx` — `ConfigService`, all static: `load`,
  `loadOverlay`, `getString`/`getInt`/`getBool`/`getDouble`, `hasKey`,
  `getStringPairs`, `drogonConfig`.
- `src/config/config-service.cc` — the TOML parse, the dotted-path resolver
  and the runtime-override map.
- `src/config/service.hxx` — `IService`: `name`, `version`, `dependencies`,
  `initialize`, `isLoaded`, `shutdown`, `health`. Header-only; it is listed
  among the sources so the target shows it.

## Rules

- Rule 25: the folder IS the module. One `argus_lib(NAME config ...)`
  declaration, explicit source lists, never `file(GLOB)`. The include root is
  `src/`, so consumers write `<config/config-service.hxx>`.
- Resolution order, in every getter and in `hasKey`: the runtime overrides
  first, then the loaded TOML. `setRuntimeString` is in-memory only and is
  never written back; the `setBool`/`setString`/`setInt`/`setDouble` family is
  the persisting one and rewrites the file surgically. A test that wants to
  pin a value for one process uses the first; a provisioning tool uses the
  second.
- A missing key is not an error: the getters answer the empty string, `0`,
  `false` or `0.0`. A boot path that must tell "absent" from "empty" asks
  `hasKey`. A malformed file IS fatal — `load` logs `LOG_FATAL` and throws
  `std::runtime_error("ConfigService: failed to parse " + path)` — so a typo
  fails the boot instead of silently defaulting every key it broke.
- `loadOverlay` refuses to run before a base `load` and replaces the
  top-level tables it carries wholesale: an overlay restates everything it
  wants to keep.
- `getStringPairs` reads one TOML table of string scalars into pairs, skipping
  non-strings. The order is the table's KEY order, not the file's — toml++'s
  table is key-ordered — so a list whose order carries meaning cannot be a
  table. Which key a value is under is the contract; where it sits in the file
  is not.

## Tests

`tests/unit/config-service-test.cc` — load, dotted paths, the absent-key
defaults, `hasKey`, the runtime override and the malformed-file refusal.
