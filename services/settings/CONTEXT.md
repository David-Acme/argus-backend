# argus-settings

The single owner-facing HTTP surface of the app's Settings area. It owns no
data: every microservice that exposes tunable behaviour publishes its own
catalog over `argus.settings.v1` (`packages/contracts/settings`) on the gRPC
server it already runs, accepting only the caller named `settings`. This
service reads those catalogs and forwards changes to their owners.

## HTTP contract

Both routes are owner-only (rule 7: `/settings` maps to no table, so
`RoleFilter` refuses every other role) and answer the `{status, info,
errors}` envelope.

`GET /settings` →

```json
{ "owners": [ { "service": "tts", "reachable": true,
  "settings": [ { "key": "tts.speed", "group": "voice", "type": "decimal",
    "level": "basic", "apply": "live", "min": 0.7, "max": 2.0, "step": 0.05,
    "choices": [], "value": "1.25", "fallback": "1" } ] } ] }
```

- `type`: `toggle` | `integer` | `decimal` | `choice` | `text`;
  `level`: `basic` | `advanced`; `apply`: `live` | `nextSession` | `restart`.
- `value` and `fallback` are the owner's canonical strings.
- Owners appear in the fixed display order llm, voice, tts, stt, vlm, guard,
  camera, notification, and only when configured (non-empty target and
  credential). An owner that does not answer is `reachable: false` with an
  empty `settings`.
- A setting whose type this service does not know (an owner newer than it) is
  left out rather than failing the page.
- A choice setting whose owner installs files per choice (argus-tts's engine,
  Pocket variant and voices) also carries `choiceStates`: `[{ "choice",
  "availability": "installed" | "installable" | "installing" | "hostOnly" |
  "failed", "sizeMb", "hostCommand" }]`. The key is absent when the owner
  reports none, so older clients see the same object as before.

`PATCH /settings/{owner}` with `{ "changes": [ { "key", "value" } ] }`:

- The DTO refuses (422, field `changes`) a missing or non-array `changes`,
  0 or more than 64 entries, an entry that is not an object with a string
  `key` and a string `value`, an empty key, a key over 128 characters or a
  value over 512.
- An owner name that is not configured → 404 `NOT_FOUND` "Unknown settings
  owner". An owner that cannot be reached, or refuses the call (deadline,
  credential, transport) → 503 `SERVICE_UNAVAILABLE`.
- Rejected changes → 422 `VALIDATION_ERROR` whose `fields` maps each rejected
  key to its reason: `unknownKey`, `invalid`, `outOfRange`, `notAChoice`, or
  `notInstalled` (a choice only the host can install, see `choiceStates`). The
  owner's registry is all-or-nothing on validation, so nothing was applied.
- A rejection with reason `writeFailed` (the owner could not persist its
  config file) → 500 `INTERNAL_ERROR`; the log names the keys.
- Success → 200 `{ "applied": [keys], "catalog": { "service", "reachable":
  true, "settings": [...] } }`, the catalog as it now stands.

## Decisions

- **Parallel reads off the loop.** `catalogsAsync()` runs `catalogs()` in a
  `BlockingTask`; `catalogs()` starts one `std::jthread` per owner and joins
  them, so the page costs the slowest owner's deadline (1.5 s by default),
  not their sum. The unit suite pins it with two owners that stall 2.5 s
  under a 1 s deadline.
- **Two clients per owner.** A catalog read uses the short list deadline; an
  update uses `settings.update_timeout_ms` (5 s), because the owner persists
  its config file and runs its change listeners before it answers.
- **Owner failures are 503 to the app.** An owner's 401 (a credential that
  drifted) would read as an expired session to the app, so every transport
  or owner refusal on the update path is reported as the owner being
  unavailable and logged with its real status.
- **Logging.** One line per applied update: user id, owner, applied keys.
  Values are never logged.

## Secrets

Each owner has its own 32-byte credential, minted by `scripts/setup.sh`
(native) and `scripts/provision-host.sh` (deploy) through
`ensure_settings_owners` in `scripts/lib/common.sh`. It is written to the
owner's caller slot (`[rpc.callers] settings` in tts, stt, vlm and llm;
`[grpc] caller_settings` in voice, and camera/notification when they adopt
it) and to `[owners.<owner>] credential` here; the target comes from the
owner's own gRPC listener. An owner with no caller slot stays unconfigured;
guard has no gRPC listener today. Existing secrets and targets are never
overwritten.

The native sandbox (`scripts/native-stack.sh`) empties the target of every
owner it does not boot, so the golden replay never depends on processes
outside the sandbox.
