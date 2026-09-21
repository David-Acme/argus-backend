# argus_contracts_camera

The camera boundary's vocabulary: the driver, record mode, event severity and
zone type enums, and the eight refusals the boundary throws.

## What this is

A CONTRACT, not a service and not a library: one `argus_contracts`
declaration, an INTERFACE target with no translation unit. The include root is
`src/`, so a consumer writes `<camera/zone-type.hxx>` and links
`argus::contracts::camera`. Two CMakeLists link it — `services/camera`, which
owns the boundary, and `packages/sync`, because a zone or an event crosses the
sync leg as the same four enums.

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
- `src/camera/zone-type.hxx` — `ZoneType` (`Monitor`, `Alert`, `Exclude`); 3
  files.
- `src/camera/camera-errors.hxx` — the eight refusals (`Forbidden`,
  `InvalidCameraId`, `CameraNotFound`, `TooManyCameraSubscriptions`,
  `TooManyViewers`, `SubscribeFailed`, `ZoneNotFound`, `CameraUnreachable`); 5
  files.

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

- `tests/unit/camera-contract-vocabulary-test.cc` — the four enums'
  round-trips, name by name.
- `tests/unit/camera-contract-catalog-test.cc` — the eight refusals as a
  pinned table, each entry's wire legality, and that no two say the same
  thing.
