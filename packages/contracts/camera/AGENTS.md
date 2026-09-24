# argus_contracts_camera

The camera boundary's vocabulary: the driver, record mode, event severity and
zone type enums, and the nine refusals the boundary throws.

## What this is

A CONTRACT, not a service and not a library: one `argus_contracts`
declaration, an INTERFACE target with no translation unit. The include root is
`src/`, so a consumer writes `<camera/zone-type.hxx>` and links
`argus::contracts::camera`. Eleven CMakeLists link it — nine in
`services/camera`: the executable's own `MODULES` list (`CMakeLists.txt:217`),
two suites, and the eight modules that page the camera tables or speak the
vocabulary (`src/camera`, `feature/{camera,camera-control,media,operator,sync,
zone}` and `src/shared/repositories`, whose link is at
`src/shared/repositories/CMakeLists.txt:8`) — plus
`services/guard`'s guard module
(`src/feature/guard/CMakeLists.txt:13`), and `argus-sync`'s `sync-transport`
module (`services/sync/src/feature/transport/CMakeLists.txt:24`, where a zone
or an event crosses the sync leg as the same four enums).

## Layout

- `src/camera/camera-driver.hxx` — `CameraDriver` (`Tapo`, `Onvif`, `Rtsp`)
  with its round-trip pair; 3 files include it. `services/camera` carries a
  differently-aimed header with the same basename
  (`src/shared/services/camera-driver/camera-driver.hxx`, the registry's driver
  abstraction); its own consumers include that one as a quoted sibling, so the
  qualified spelling `<camera/camera-driver.hxx>` is this file.
- `src/camera/camera-record-mode.hxx` — `CameraRecordMode` (`Events`,
  `Continuous`); 3 files.
- `src/camera/event-severity.hxx` — `EventSeverity` (`Info`, `Warning`,
  `Critical`); 4 files.
- `src/camera/identity-state.hxx` — `IdentityState` (`Known`, `Unrecognized`,
  `Unobservable`) with its round-trip pair and a fail-closed parse: this is the
  `identityState` field of every track-bound person object, camera's matcher
  writes the spelling and guard's policy reads it back, so the enum is named by
  two services -- and until Phase 2 step 4 each of them carried a private copy,
  which is the drift section 2.4 rule 6 exists to stop. 2 files.
- `src/camera/zone-type.hxx` — `ZoneType` (`Monitor`, `Alert`, `Exclude`); 3
  files.
- `src/camera/camera-errors.hxx` — the nine refusals (`Forbidden`,
  `InvalidCameraId`, `CameraNotFound`, `TooManyCameraSubscriptions`,
  `TooManyViewers`, `SubscribeFailed`, `ZoneNotFound`, `CameraUnreachable`,
  and the answer a change that could not be recorded gives); 6 files.

## Rules

- The four enums are wire values: `<enum>ToString` is what the app sends and
  what the database stores. Each is `uint8_t` with an explicit first
  enumerator, and each has a `<enum>ToString`/`<enum>FromString` pair whose
  strings are frozen — `"tapo"`, `"events"`, `"info"`, `"monitor"`.
- This package is the C++ half of the contract, not the schema half. The
  protos are `packages/contracts/proto/argus/camera/v1/*.proto`, compiled by
  the clients that speak them (`packages/clients/camera` for `sync.proto`,
  `packages/clients/camera-actions` for `actions.proto`); `camera.proto`,
  `stream.proto` and `zone.proto` are schema today, with no client compiling
  them.
- Rule 25: the folder IS the module. One `argus_contracts(NAME camera ...)`
  with an explicit source list, never `file(GLOB)`.

## Tests

- `tests/unit/camera-contract-vocabulary-test.cc` — the five enums'
  round-trips, name by name, plus the fail-closed half of `IdentityState`: an
  absent or invented spelling reads as `unrecognized`, never as `known`.
- `tests/unit/camera-contract-catalog-test.cc` — the nine refusals as a
  pinned table, each entry's wire legality, and that no two say the same
  thing.
