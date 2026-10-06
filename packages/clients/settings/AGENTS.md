# argus_clients_settings

The `argus.settings.v1` wire seen from the caller's side: read an owner's
settings catalog and send it changes.

## What this is

A CLIENT module: one `argus_clients(NAME settings ...)`, a STATIC library
whose include root is `src/`, so a consumer writes
`<settings/settings-client.hxx>` and links `argus::clients::settings`. It
calls `argus_settings_rpc_contract()` (owned by `packages/contracts/settings`)
and links `argus::contracts::settings-wire` PRIVATE, so no protobuf header
reaches a consumer through it. Its one consumer is `services/settings`.

## Layout

- `src/settings/settings-client.hxx` — `SettingsClientConfig` (`target`,
  `credential`, `timeout`, 5 s by default), `SettingsCatalog` (`service` and
  `SettingEntry` rows), `SettingsUpdateReply` (`applied`, `rejected`,
  `catalog`) and `SettingsClient` with `list()` and `update(changes)`. The
  vocabulary is `lib/config`'s settings registry types (`SettingSpec`,
  `SettingEntry`, `SettingChange`, `SettingRejection`), the same ones an
  owner's registry is built from.
- `src/settings/settings-client.cc` — the stub, the deadline, the caller
  credential (`x-argus-credential` through `addCallerCredential`) and the
  wire-to-vocabulary mapping.

- `SettingsClient` also carries the component and module data calls
  (`componentStates`, `installComponent`, `cancelComponent`,
  `removeComponent`, `moduleDataSummary`, `purgeModuleData`,
  `verifyOwnerPin`); each returns `std::nullopt` when the owner answers
  `UNIMPLEMENTED` and throws like the other calls otherwise.
- `ModulesClient` (same config struct) reads argus-settings'
  `Modules/ModuleStates`: `moduleStates()` → `ModuleStatesReply`. Services
  read it at boot with their `[modules] target`/`credential`.

## Rules

- The constructor is the gate: an empty target, an empty credential, a
  non-positive timeout or one over two minutes throws
  `ResponseException(400, SettingsErrors::InvalidRequest)` before dialling.
- A failed call throws `argus::response::fromRpcStatus(status)`: the owner's
  own refusal when it sent one (401 for a wrong credential, 400 for an empty
  or oversized update), the transport's otherwise (503 unreachable, 504
  deadline).
- A setting whose wire type is unspecified is dropped from the catalog; an
  unspecified level reads as advanced, an unspecified apply as restart and an
  unspecified rejection reason as invalid.
- One channel per client; the caller owns how many clients it keeps per owner.

## Tests

- `tests/unit/settings-client-test.cc` — the constructor gate, the catalog
  mapping and an applied and refused update against an in-process owner
  (`SettingsRpcService` over a `SettingsRegistry` on `127.0.0.1:0`), and the
  refusals: a wrong credential (401), an empty update (400) and a dead owner
  (503 or 504).
