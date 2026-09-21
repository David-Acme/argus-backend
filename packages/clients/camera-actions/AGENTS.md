# argus_clients_camera-actions

The guard -> camera action edge seen from the caller's side: announce, alarm,
siren, crop and listen, each answered with one classified outcome.

## What this is

A module, not a service: one `argus_clients(NAME camera-actions ...)`, a STATIC
library whose include root is `src/`, so a consumer writes
`<camera/camera-action-client.hxx>` and links
`argus::clients::camera-actions`. It compiles one proto
(`argus/camera/v1/actions.proto`) and one source
(`src/camera/camera-action-client.cc`) — measured deviation: the source
directory is `src/camera/`, not `src/camera-actions/`, while the target and the
include prefix follow the package name. Five link lines in three CMakeLists
take it: `argus-guard` (`services/guard/CMakeLists.txt:142`) and
`guard-assessment-live-test` (:250), `argus_guard`
(`services/guard/src/feature/guard/CMakeLists.txt:21`, the feature module's
`DEPENDS`), `camera-core` (`services/camera/CMakeLists.txt:268`) and
`camera-action-rpc-test` (:483). The two service CMakeLists also add the
package to their own tree by path (`services/guard:85`, `services/camera:162`).
13 C++ files include the header: the in-package suite, `services/camera`'s
action-rpc suite, and eleven guard files (`main.cc`, `guard-service.cc`,
`guard-assessment.cc` and the eight guard unit suites).

## Layout

- `src/camera/camera-action-client.hxx` — `using CameraCommandOutcome =
  argus::camera::v1::CommandOutcome;` at global scope (the protoc enum is this
  surface's vocabulary, a second measured deviation from §2.3), the input
  structs `CameraAnnounceInput`, `CameraAlarmInput`, `CameraSirenInput`,
  `CameraPersonCropInput`, `CameraListenInput` and `CameraCrop`,
  `CameraCommandResult` with its seven predicates,
  `CameraActionClientConfig` (`target`, `credential`), and `CameraActionClient`
  with `announce`, `alarm`, `setSiren`, `personCrop` and `listen`; 13 files
  include it.
- Nothing else: `find packages/clients/camera-actions -type f` returns
  CMakeLists.txt, the two sources, the suite and this file. No `details/`
  directory: the channel, deadline and credential live inline in the `.cc`.

## Rules

- Rule 25: the folder IS the module. One
  `argus_clients(NAME camera-actions ...)` with an explicit source list, never
  `file(GLOB)`.
- The include prefix is load-bearing: `<camera/camera-action-client.hxx>`.
- What a consumer sees: `CameraActionClient` and `CameraCommandResult` — one
  result type that keeps the transport status apart from the command outcome,
  so a retryable transport failure is never mistaken for a rejected command or
  for a completed one. What it must not see: no stub, no channel, no URL, no
  retry policy.
- The refusals stay local: `announce`, `alarm` and `setSiren` answer
  INVALID_ARGUMENT `"camera id is required"` for a non-positive camera id,
  `listen` answers `"camera id and command id are required"` for a non-positive
  camera id or an empty command id, and `personCrop` answers nullopt — no RPC is
  attempted, which the suite pins against a dead target, where the same call
  with a valid id comes back UNAVAILABLE.
- The outcome is read off the wire, never inferred: an explicit ack `outcome`
  wins; then duplicate -> DUPLICATE_SUCCEEDED, accepted -> SUCCEEDED, detail
  `in_flight` -> IN_FLIGHT, `indeterminate` -> INDETERMINATE,
  `command_id_conflict` -> CONFLICT; a silent ack is REJECTED. A transport
  failure maps by code: UNAVAILABLE and DEADLINE_EXCEEDED -> RETRYABLE_FAILED;
  INVALID_ARGUMENT, FAILED_PRECONDITION, NOT_FOUND, PERMISSION_DENIED,
  UNAUTHENTICATED, ALREADY_EXISTS and RESOURCE_EXHAUSTED -> REJECTED; anything
  else -> INDETERMINATE. `retryable()` is exactly those two transport codes or
  a RETRYABLE_FAILED outcome, so an ambiguous failure is never repeated.
- On the wire: one capability credential, `x-argus-credential`, sent only when
  non-empty (`addCallerCredential`); no fleet secret and no x-argus-* identity —
  measured, the `.cc` calls `setDeadline` and `addCallerCredential` and nothing
  else from the shared base. One deadline, `kCallTimeoutMs` = 60000 ms, a
  `constexpr` in the `.cc`. The channel is plaintext (`makeChannel` is
  `InsecureChannelCredentials`).
- Config: `camera.actions_target` and `camera.actions_credential`
  (`services/guard/src/main.cc:149-156` is the only reader, and it skips the
  client entirely when the target is empty). One file declares the target:
  `argus-deploy/config.guard.toml:137` (`argus-camera:7036`). No file declares
  the credential — measured, no `.toml` in the tree carries the key — so on that
  tree the credential is empty and no `x-argus-credential` header is sent.

## Tests

- `tests/unit/camera-action-client-test.cc` — three cases: the local refusals
  with the dead target as their control; the ack reading (explicit outcome over
  accepted+duplicate, duplicate, accepted with its detail, the two detail-only
  readings `in_flight` and `command_id_conflict`, and a silent ack as a
  rejection); the transport classification (NOT_FOUND as rejected, INTERNAL as
  indeterminate).
- The CMakeLists registers this suite as `camera-action-client-test`, with
  `EXCLUDE_FROM_ALL FALSE` because the folder is pulled in
  `EXCLUDE_FROM_ALL`. Because that folder is added by two of the gate's
  projects, ctest collects the suite twice — once per project that pulls the
  package in.
