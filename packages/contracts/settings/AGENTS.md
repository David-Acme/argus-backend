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
  nothing send none.
- `src/settings/settings-errors.hxx` — `Unauthorized` 401,
  `InvalidRequest` 400, `Unavailable` 503, `UnknownService` 404.
- `src/settings/settings-rpc.{hxx,cc}` — `SettingsRpcService`, a callback
  service over a `SettingsRegistry` (from `lib/config`). Every call needs a
  caller credential; `Update` takes 1 to 64 changes and answers with what
  was applied, what was refused and why, and the catalog as it now stands.
  The registry (`lib/config`) asks the owner's `describeChoices` provider
  for the states on every `List`, and refuses a change to a `HOST_ONLY`
  choice with `REJECTION_REASON_NOT_INSTALLED` (re-sending the current value
  is still accepted, so the owner can retry an install).

## Rules

- A service exposes only what its registry declares. Paths, ports,
  credentials, targets and secrets never enter a catalog.
- Labels and help text are the app's: the wire carries keys, never prose.
- A choice's host command is a repo-relative command line for the operator
  (`services/tts/scripts/provision.sh --variant es-quality`), never an
  absolute path of the installation, a credential or a URL.
