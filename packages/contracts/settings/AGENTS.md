# argus_contracts_settings

The owner settings boundary: the schema every settings owner answers with,
its refusals, and the one server adapter that turns a `SettingsRegistry`
into that schema.

## What this is

A CONTRACT with a wire half, like `tts`: the header-only
`argus::contracts::settings` vocabulary target, and
`argus_settings_rpc_contract()`, the CMake function that declares
`argus::contracts::settings-wire` from `settings.proto` together with
`SettingsRpcService`. A service that publishes settings includes this
package, calls the function, links the wire target and registers one
`SettingsRpcService` on the gRPC server it already runs. The include root
is `src/`: `<settings/settings-rpc.hxx>`, `<settings/settings-errors.hxx>`.

## Layout

- `settings.proto` — `package argus.settings.v1`: one `Settings` service
  with `List` and `Update`. A `Setting` carries key, group, type, level
  (basic or advanced), when it applies (live, next session, restart),
  range, choices, fallback and current value, always as the canonical
  string the registry produced. A choice setting may also carry
  `choice_states` (field 12, additive): for each choice whose files an owner
  installs, its `ChoiceAvailability` (`INSTALLED`, `INSTALLABLE` on demand,
  `INSTALLING`, `HOST_ONLY`, `FAILED`), its size in MB and the repo-relative
  command an operator runs on the host to install it. Owners that install
  nothing send none. `unit` (13) is the owner's short unit code (`ms`, `s`,
  `px`, `tokens`, `threads`, `layers`, ...; empty = unitless) and
  `pending_restart` (14) says that a restart key's value in the file differs
  from the one the process booted with.
- `SettingsCatalog` also carries `config_path` (3), the absolute path of the
  TOML file the owner loaded (read-only metadata; the settings service may
  replace it with the host path it is configured with), `profile` (4), the
  `ProfileMarker` stored in the owner's own file (`[settings_profile]`: id,
  `ProfileOrigin` recommended/owner/reverted, applied_at, keys), always sent
  by an owner built with it (its absence means an older owner), and
  `capabilities` (5), e.g. `gpu` when the owner's engine can offload.
- `UpdateSettingsRequest.profile` (2) asks the owner to record a marker
  after the changes; it is written only when no change was refused, and
  `UpdateSettingsResponse.profile_recorded` (4) says whether it was. An
  update may carry zero changes when it carries a marker.
- `src/settings/settings-errors.hxx` — `Unauthorized` 401,
  `InvalidRequest` 400, `Unavailable` 503, `UnknownService` 404.
- `src/settings/settings-rpc.{hxx,cc}` — `SettingsRpcService`, a callback
  service over a `SettingsRegistry` (from `lib/config`). Every call needs a
  caller credential; `Update` takes 0 to 64 changes (0 only with a marker) and answers with what
  was applied, what was refused and why, and the catalog as it now stands.
  The registry (`lib/config`) asks the owner's `describeChoices` provider
  for the states on every `List`, and refuses a change to a `HOST_ONLY`
  choice with `REJECTION_REASON_NOT_INSTALLED` (re-sending the current value
  is still accepted, so the owner can retry an install).

- Component and module calls (additive): `ComponentStates`,
  `InstallComponent`, `CancelComponent`, `RemoveComponent` (a
  `ComponentSpec`: id, `download`/`provisioned`, files with path, url, size
  and SHA-256, host command → a `ComponentStatus`: state installed, missing,
  installing, failed or host_only, bytes present/total, `ready`, host command,
  reason), `ModuleDataSummary` and `PurgeModuleData` (by module id →
  `[{kind, count}]` and bytes, or purged + reason) and `VerifyOwnerPin` (user
  id + PIN → no_pin, accepted, required, invalid, locked). An owner registers
  nothing for them by default and answers `UNIMPLEMENTED`; it opts in with
  `SettingsRpcService::attachComponents(ComponentHost&)`,
  `attachModuleData(ModuleDataHost&)` and `attachOwnerPin(OwnerPinHost&)`.
  The C++ vocabulary is header-only in
  `src/settings/component-vocabulary.hxx`; `component-wire.{hxx,cc}` maps it
  to and from the wire.
- `DiskComponentHost` (`src/settings/component-host.{hxx,cc}`) is the shared
  owner side: the state comes from the files under the owner's models root (a
  download is complete when its size matches, a provisioned file when it
  exists; `<file>.part` counts as bytes present), installs run one
  `std::jthread` per component through the `ComponentFetch` the owner passes
  (the `lib/http` downloader: `.part` + sidecar, `Range` resume, SHA-256,
  atomic rename; it returns an empty string or a reason code), cancel stops
  the thread through its `stop_token` and keeps the partial file, remove
  deletes a download's files and partials. Specs are refused (`INVALID_ARGUMENT`)
  when the id is not one the owner declared, a path is absolute or climbs out,
  or a download lacks a `https` URL or a SHA-256.
- `Modules` is a second service in the same proto, served only by
  argus-settings: `ModuleStates` → the enabled set
  (`{id, enabled, lifecycle, data_purged_at}`, `version`, `settled`).

## Rules

- A service exposes only what its registry declares. Paths, ports,
  credentials, targets and secrets never enter a catalog as settings. The
  one path on the wire is `config_path`, the owner's own config file, which
  the owner-only Configuración shows so the operator knows which file a key
  lives in; it is never writable through the wire.
- Labels and help text are the app's: the wire carries keys, never prose.
- A choice's host command is a repo-relative command line for the operator
  (`services/tts/scripts/provision.sh --variant es-quality`), never an
  absolute path of the installation, a credential or a URL.
