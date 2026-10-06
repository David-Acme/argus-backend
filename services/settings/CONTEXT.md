# argus-settings

The single owner-facing HTTP surface of the app's Settings area and the
module manager (see "Modules"). Apart from the module state in `settings.db`
it owns no data: every microservice that exposes tunable behaviour publishes its own
catalog over `argus.settings.v1` (`packages/contracts/settings`) on the gRPC
server it already runs, accepting only the caller named `settings`. This
service reads those catalogs and forwards changes to their owners.

## HTTP contract

Every route is owner-only (rule 7: `/settings` maps to no table, so
`RoleFilter` refuses every other role) and answers the `{status, info,
errors}` envelope. The profile routes are described in "Profiles" below.

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
  camera, notification. All eight are listed: one without a target and a
  credential is `configured: false` (and `reachable: false`), so the app can
  name what is not connected instead of showing an empty page. An owner that
  does not answer is `reachable: false` with an empty `settings`.
- Each owner also carries `configFile` (the TOML file its keys live in:
  `[owners.<owner>] config_file` when set, which provisioning fills with the
  host path in a deploy, otherwise the absolute path the owner loaded),
  `capabilities` (`["gpu"]` when its engine can offload to a GPU) and
  `profile`, the marker it keeps in its own file (`{ "id", "origin":
  "recommended" | "owner" | "reverted", "appliedAt", "keys" }`, or null when
  no profile was ever applied there or the owner predates markers).
- Each setting also carries `unit` (a code the app translates: `ms`, `s`,
  `px`, `tokens`, `threads`, `layers`, `steps`, `frames`, `bytes`, `MB`,
  `days`, `entries`, `chars`, `turns`, `perHour`; empty = none; declared by
  the owner or derived from the key's suffix by the registry) and
  `pendingRestart` (a restart key whose value in the file is not the one the
  owner booted with).
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

## Profiles

The owner picks one of three profiles, "Máximo rendimiento"
(`performance`), "Equilibrado" (`balanced`) and "Máxima calidad"
(`quality`), and every owner it touches is set at once. Fine-tuning per key
stays where it was; a profile is a starting point, not a mode.

**Profiles are data.** `profiles.json` beside `config.toml.example` maps
profile → owner → key → canonical value, and holds the recommendation
rules. `[settings] profiles_path` names it (default `profiles.json`,
relative to the working directory). The native run starts in
`services/settings/` and finds it there, `scripts/native-stack.sh` writes the
repository file's absolute path into the sandbox copy, and the image bakes it
at `/opt/argus/profiles.json` (Dockerfile), so the deploy config's relative
default resolves without a compose mount. Changing a profile is an edit of
that file and a restart (native) or an image rebuild (deploy).
`infra/profile-file.cc` validates it at boot: ids `[a-z0-9-]`, unique; known
owner names; 1..64 string values per owner; rules that name an existing
profile; a fallback that exists. A missing or invalid file is logged with the
place of the problem and the profile routes answer 503; the rest of the
service keeps working.

### The values and why

Only keys that exist in an owner's catalog and trade speed against quality
on measured evidence are in the file. Every number below comes from the
owner's own CONTEXT.md, measured on the reference host (Ryzen 7 5825U,
8 cores / 16 threads, 32 GB).

| Owner | Key | performance | balanced | quality | Evidence |
|---|---|---|---|---|---|
| tts | `tts.engine_es` | pocket | pocket | pocket | Pocket fast speaks after 30/35 ms against Supertonic's 1172/3993 ms (short/long); the owner chose Pocket quality by ear. |
| tts | `tts.engine_en` | pocket | pocket | pocket | Pocket en 26/32 ms first audio against 1028/3613 ms, and ahead by the owner's ear. |
| tts | `tts.pocket_variant_es` | fast | fast | quality | fast: RTF 0.097/0.103, 994 MB resident; quality (24 layers): RTF 0.297/0.320, 78/113 ms, 1.40 GB, the same WER on the test sentences, and the owner's choice ("utilizar calidad máxima"). |
| tts | `tts.pocket_voice_es` | — | — | jean | The owner's voice for the house ("una voz M de Jean"); WER 0.00 on both Spanish models. |
| vlm | `vision.max_input_px` | 256 | 384 | 512 | 256: 1.05–1.27 s and the same captions on the test scenes; 384: 1.35–1.70 s, the default because camera subjects are small; 512: 1.38–1.98 s, more detail. |
| vlm, llm | `vision.gpu_layers`, `llm.gpu_layers` | — | — | 999 where the engine reports a GPU | VLM 0.61 s instead of 1.09 s per sentence on the Vega 8 iGPU (1.8x); see GPU offload. |

- **balanced keeps Spanish on `fast`.** It is recommended for 4–7 cores,
  where `ThreadBudget::ttsThreads()` gives Pocket half the hardware threads:
  the 24-layer model's RTF 0.30 at 8 threads becomes roughly 0.6 at 4, while
  STT, the LLM's prefill and decode run on the same cores during a call.
  The fast model costs a third of that and 400 MB less, with the same WER.
- **Spanish quality stays one tap away in every profile.** Every profile
  keeps `tts.engine_es = pocket`, so `tts.pocket_variant_es = quality` remains
  the selectable, effective choice; a profile never removes files.
- **`jean` only in quality.** The voice does not trade speed for quality,
  and it is CC BY-NC (opt-in install). Where it is not installed and only the
  host can install it, the preview shows the host command and the apply
  reports `notInstalled` for that key while the rest applies.
- **Left out, on purpose.** `stt.engine`: only `nemo_transducer` is measured
  (RTF 0.03–0.05), and whisper/canary rebuild the recognizer per language
  switch (1378 ms against 96 ms), so no profile should move it. The LLM keys:
  `context_size` only costs RAM as history grows (the history cap is the real
  lever), `kv_type` is already chosen by RAM, the thread keys default to
  `ThreadBudget`, and none has a speed/quality measurement. `vision.gpu_layers`
  and `llm.gpu_layers` are set only by Máxima calidad and only where the
  engine reports a GPU (see "GPU offload"). `vision.max_tokens`: the
  image encoder dominates (a one-word answer costs 60–75 % of a sentence).
  `vision.image_max_tokens`: below 256 the model stops reading text.
  `tts.quality`/steps only shape Supertonic, the fallback engine. Voice turn
  keys (VAD) are about the room, not the hardware. Guard, camera and
  notification keys are security and privacy: never in a profile.

### GPU offload

`vision.gpu_layers` and `llm.gpu_layers` (-1..999, -1 = the CPU probe, 999 =
every layer) are restart keys. "Máxima calidad" sets both to 999 through a
`withCapability.gpu` block in `profiles.json`: those keys join the profile only
for an owner whose catalog reports the `gpu` capability, which argus-vlm and
argus-llm declare when `llama_supports_gpu_offload()` finds a Vulkan device
(in a container, only with `/dev/dri` passed through). Elsewhere the profile
is exactly what it was. Measured on the reference host's Radeon Vega 8: the
VLM describes in 0.61 s instead of 1.09 s per sentence (1.8x, services/vlm
CONTEXT.md). The LLM, measured 2026-10-03 with the prod binary on the same
iGPU (load 6-8, a 258-token prompt, 160 tokens out, three runs): prefill
101 -> 182 tok/s (1.8x), decode 22.6 -> 25.1 tok/s (1.1x), so the turn's
first token comes sooner and the CPU cores stay free for STT and TTS. Performance and
Equilibrado never set them.

### First run: the recommendation applied by default

Nothing was ever set on a fresh installation, so argus-settings applies the
recommended profile itself, once per owner. **The state lives in each owner's
own file**, in a `[settings_profile]` section the registry writes (`id`,
`origin`, `applied_at`, `keys`), next to the values it describes: argus-settings
still owns no data, there is no extra volume, an owner added later gets its
own first run, and a config file reset from its template starts over.

- `FirstRunService` (a `jthread`, registered as a shutdown drain) waits 5 s
  after boot, then every `[settings] first_run_interval_s` (30 s) reads the
  catalogs of the recommended profile's owners until each is settled.
  `[settings] first_run = false` turns it off.
- An owner is settled when its marker has an origin, when it is not
  configured, or when it answers without a marker (it predates them: an old
  owner is never written to, since it could not remember that it was).
- Otherwise the plan (`settings_profile::firstRunPlan`) takes only keys that
  still hold their fallback (a value someone set by hand, in the file or the
  app, is kept), that the profile changes, and whose choice is already
  installed: the first run never downloads a model or a voice. The write
  carries the marker `origin = recommended` with the applied keys, recorded
  only when no key was refused; refused keys are dropped in rounds as in an
  apply, and a marker-only write records the owner when nothing changed.
- Later changes are never overwritten: once the marker has an origin, the
  first run never touches that owner again.
- `GET /settings/profiles` adds `firstRun`: `{ "profile", "state": "applied"
  | "reverted", "appliedAt", "owners": [ { "service", "keys" } ] }` from the
  markers, or null. The app shows what was applied and offers to undo it.
- `POST /settings/profiles/recommended/revert` (body `{}`) undoes it: for each
  owner whose marker is `recommended`, the keys it applied that still hold the
  profile's value go back to their fallback (a key changed since is kept), and
  the marker becomes `reverted`, so the first run stays done. It answers like
  an apply; 404 `NOT_FOUND` when no owner carries a recommended marker.
- Applying a profile by hand records `origin = owner` on every owner it
  reaches, including a marker-only write where nothing changed.

### Recommendation

`infra/hardware-facts.cc` reads `HardwareProbe::get()` once at boot
(`argus::hardware-profile`, the ncnn-free probe): physical cores, logical
threads, total RAM in GB to a tenth, the widest vector ISA
(`avx512`/`avx2`/`neon`/`baseline`) and the GPU's video-accel API from
`/dev/dri` (`vaapi`/`qsv`/`nvdec`/`none`). The rules in the file are tried in
order; the first the host meets wins, otherwise the fallback:

| Profile | Cores | RAM | Vector ISA |
|---|---|---|---|
| quality | ≥ 8 | ≥ 14 GB | AVX2, AVX-512 or NEON |
| balanced | ≥ 4 | ≥ 7 GB | AVX2, AVX-512 or NEON |
| performance | otherwise | | |

- 8 cores is the host every quality number was measured on. 4 cores is half
  of it, where the fast variant still runs at roughly RTF 0.2.
- RAM floors are the 16 and 8 GB classes as the kernel reports them: firmware
  and iGPU carve-outs take 0.5–2 GB (the reference host's 32 GB reads 30.7).
  The only measured RAM difference between profiles is the 24-layer model's
  +0.4 GB; the floors guard the whole fleet of engines, not this delta.
- Every measurement ran on AVX2. Without a vector ISA the int8 and Q4 kernels
  fall back to scalar code, so such a host gets the lightest profile.
- The GPU the probe reports here is the settings process's own view (none in
  its container); whether an engine offloads is the engine's `gpu`
  capability, see "GPU offload".
- `reason` names what held back the profile above the recommended one:
  `meets` (the top rule), `cores`, `ram` or `isa`; `rule` is the rule met
  (null for the fallback) and `missed` the one above it. `rules` and
  `fallback` repeat the whole ladder, so every card can say what it asks for
  and whether this machine is above or below it. The app writes the
  sentences; the wire carries codes and numbers.
- In a container the probe sees the host's cores and RAM (a CPU quota or a
  `mem_limit` does not change them), and `gpu` reads `none` unless `/dev/dri`
  is passed through.

### HTTP contract

`GET /settings/profiles` →

```json
{ "profiles": [ { "id": "quality", "labelKey": "quality", "current": false,
    "owners": [ { "service": "tts", "reachable": true, "changes": [
      { "key": "tts.pocket_variant_es", "from": "fast", "to": "quality",
        "changed": true, "apply": "live",
        "install": { "availability": "installable", "sizeMb": 355,
                     "hostCommand": "" } } ] } ] } ],
  "recommendation": { "profile": "quality", "reason": "meets",
    "hardware": { "cores": 8, "threads": 16, "ramGb": 30.7, "isa": "avx2",
                  "gpu": "vaapi" },
    "rule": { "profile": "quality", "minCores": 8, "minRamGb": 14,
              "vectorIsa": true },
    "missed": null,
    "rules": [ { "profile": "quality", ... }, { "profile": "balanced", ... } ],
    "fallback": "performance" } }
```

- Every key of the profile is listed in the owner's catalog order, with its
  current value (`from`), the target (`to`) and `changed` (compared by the
  setting's type, so `1.25` and `1.250` are the same decimal). `install` appears when the target choice is not installed, with
  the owner's own `choiceStates` entry. The size shown for a voice is the one
  of the variant configured now.
- An owner that does not answer, or is not configured, is `reachable: false`
  with `from: null` and no `apply`. A key the owner's catalog lacks is left
  out of the preview and refused as `unknownKey` on apply.
- `current` is true when every owner answers and every key already holds its
  value.

`POST /settings/profiles/{id}/apply` (body `{}`, `ValidJsonFilter` stays in
the chain) → 200 even when partial:

```json
{ "profile": "performance",
  "summary": { "applied": 1, "unchanged": 2, "rejected": 0, "unreachable": 1 },
  "owners": [ { "service": "tts", "reachable": true, "results": [
      { "key": "tts.pocket_variant_es", "from": "quality", "to": "fast",
        "status": "applied" } ], "catalog": { "service": "tts", ... } },
    { "service": "vlm", "reachable": false, "results": [
      { "key": "vision.max_input_px", "from": null, "to": "256",
        "status": "unreachable" } ] } ] }
```

- Unknown profile → 404 `NOT_FOUND`; no profile file → 503.
- The catalogs of the profile's owners are read in parallel, then one update
  per owner runs in parallel (`SettingsGatewayService::write`, a `jthread`
  each, the update deadline each), the same shape as `catalogs()`.
- Only keys whose value differs are sent: re-sending a value would rerun an
  owner's change listeners (the VLM empties its caption cache). A target the
  catalog marks `hostOnly` is refused as `notInstalled` before any write.
- An owner's registry refuses a whole update when one key fails validation.
  The apply therefore runs in rounds (`ProfileApplication`): the refused keys
  are settled with their reason and the rest is sent again; a round that
  makes no progress ends the owner. A `writeFailed` key is final and its
  siblings stay applied.
- `status`: `applied`, `unchanged`, `rejected` (with `reason`, the same codes
  as the PATCH route) or `unreachable` (the owner missed the read or the
  write). `catalog` is the owner's catalog after the last write, so the app
  replaces its rows without a second read.
- One log line per apply: user id, profile id, and per owner the applied,
  refused and unreachable keys, never a value.
- The golden replay declares `recommendation` volatile for
  `GET /settings/profiles` (`scripts/golden-http.py`): it describes the host
  that answers.

## Secrets

Each owner has its own 32-byte credential, minted by `scripts/setup.sh`
(native) and `scripts/provision-host.sh` (deploy) through
`ensure_settings_owners` in `scripts/lib/common.sh`. It is written to the
owner's caller slot (`[rpc.callers] settings` in tts, stt, vlm, llm and
guard; `[grpc] caller_settings` in voice, camera and notification) and to `[owners.<owner>] credential` here; the target comes from the
owner's own gRPC listener (`[rpc] address` for the `rpc` owners, `[server]
grpc_port` for voice, camera and notification). When the owner's config
lacks the listener (a config older than its template: an empty guard `[rpc]
address`, a notification file without `grpc_port`), the template's value is
used, and an empty guard address is written from the template. An owner
with no caller slot stays unconfigured. Existing secrets and targets are
never overwritten.

The native sandbox (`scripts/native-stack.sh`) empties the target of every
owner it does not boot, so the golden replay never depends on processes
outside the sandbox, and pairs the three it does boot (guard on 7139,
camera on 7036, notification on 7038) through `ensure_settings_owners` in
its `stack` mode, on `prepare` and on every `restart` of settings or one of
them. `GET /settings` on the sandbox therefore lists guard (40 keys), camera
(17) and notification (10) as reachable; the golden fixture of that route
has to be recorded with them.

## Modules

argus-settings is the module manager of the selectable-modules plan
(`docs/history/plans/modules-and-welcome-plan.md`): the catalog, dependency
resolution, the hardware check, the persistent job queue, the module
lifecycle, the REST surface and the progress events. It is the one piece of
this service that owns data: `settings.db` (`[modules] db_path`), created at
boot from `database/schema.sql` (`[modules] schema`, statements are
`IF NOT EXISTS`, so a boot never resets it). Each other service still owns
its own files: settings asks an owner over the settings wire to fetch, stop,
remove or purge, it never touches another models dir or database (rule 27).

### The catalog

`modules.json` (`[modules] catalog_path`, baked at `/opt/argus/modules.json`
in the image) holds `components` and `modules`. `infra/module-catalog-file.cc`
validates it at boot like `profiles.json`: ids `[a-z0-9-]`, unique per kind;
exactly one `core` module, which requires nothing; every required module and
every component known, no dependency cycle (`services/module-resolver.cc`);
a route prefix gated by one module only; `coming_soon` installs nothing;
`dataOwners` are settings owners or the data-only owners `identity`,
`productivity` and `sync`. A component
names its `owner`, its `source` (`download`: the owner fetches it;
`provisioned`: only the host produces it and `hostCommand` says how), the
`ramMb` it needs loaded and its files (`path` relative to the owner's models
root, `sizeBytes`, and for a download a `https` `url` pinned to a 40-hex
revision or a release asset plus its `sha256`). A missing or invalid file is
logged with the place of the problem and every `/modules` route answers 503
`SERVICE_UNAVAILABLE`; the settings routes keep working.

The shipped components and where their numbers come from:

| Component | Owner | Source | Files and size | Pin |
|---|---|---|---|---|
| `voice-stt` | stt | provisioned | NeMo FastConformer transducer int8 (encoder, decoder, joiner, tokens), 138 474 010 B | tarball SHA-256 in `services/stt/scripts/provision.sh` |
| `voice-tts` | tts | provisioned | Pocket `es-fast` + `en` bundles, 251 636 723 B | exported locally by `services/tts/scripts/provision.sh` |
| `llm` | llm | provisioned | `LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf`, 695 755 488 B | SHA-256 `bb741e…` (root AGENTS.md 13e) |
| `vision` | vlm | download | `LFM2.5-VL-450M-Q8_0.gguf` 379 219 104 B + `mmproj-LFM2.5-VL-450m-F16.gguf` 189 126 080 B | revision `1abed04b…` and the two SHA-256 of `services/vlm/scripts/provision.sh`; sizes from the Hugging Face LFS metadata of that revision |
| `detector` | camera | provisioned | `yolo26n.param` 26 150 B + `yolo26n.bin` 9 736 936 B | NCNN export of `services/camera/scripts/provision.sh` |

Sizes of the provisioned files are the ones on the reference host. RAM per
module is an estimate, not a measurement of a loaded fleet (no service was
restarted to measure it): core 4 GB minimum / 8 GB recommended (Pocket TTS
measured 994 MB resident in services/tts CONTEXT.md, the 696 MB Q4 LLM plus
its KV cache about 1.3 GB, STT about 0.45 GB, and the always-on services; 8 GB
is the "balanced" floor of the profiles), surveillance 1.5 / 3 GB (the 568 MB
Q8 VLM plus its projector compute at 384 px, YOLO26n and the decode
pipelines), productivity 64 / 128 MB. Core and surveillance recommend AVX2,
the ISA every speed number was measured on.

### Roles and intro (the context plan)

A module also declares `roles`, the roles it brings (`surveillance` brings
`guard`; the others none, and `core` may not: the Owner, Resident and Guest
belong to the household and no module owns them), and an `intro` per language
(`{ "es": { "what", "examples": [2-3] }, "en": { ... } }`: what the module is and a
few things to say to Argus). The parser refuses a role that is `owner`, a role
two modules bring, an id that is not `[a-z0-9-]` and more than five examples;
`intro` and `roles` are optional so a catalog written before them still loads,
and the shipped one carries both for every module (a test pins it). A role whose
module is not `active` is inactive everywhere
(`packages/lib/auth/CONTEXT.md`, "Roles per module"): the user keeps it, the
server grants core only and the app shows the inactive-role screen.

Both reach every service with the enabled set: each entry of the `enabled`
event and of `ModuleStates` carries `roles`, `name`, `summary` and `intro` in
both languages besides `id`, `enabled`, `lifecycle` and `dataPurgedAt`, so a
service's gate and argus-sync's context can name a module without asking. The
Owner's module JSON (`GET /modules`, the `module` event) gains `roles` and
`intro` (in the caller's language). `Modules/OwnerCatalog` returns the Owner's
whole list as one JSON array (`modules_json`, names in Spanish like the `module`
event, plus the `version`): argus-sync reads it when an Owner connects and on
each change to fill `ownerCatalog` of the context, so the app stops polling
`GET /modules`.

### Lifecycle and the enabled set

`module_state` keeps one row per module: `lifecycle` (`not_installed`,
`active`, `disabled`, `uninstalled_data_kept`) and `data_purged_at` (unix
ms, 0 = never). A module is enabled exactly when its lifecycle is `active`;
`core` is always active. The enabled set's `version` is the id of the last
`module_audit` row that changed a lifecycle (`adopted`, `enabled`,
`disabled`, `rolled_back`, `removed`, `purged`), so it only grows.

The first boot of `settings.db` (no `module_state` rows) adopts what exists,
so an upgraded installation keeps its cameras: core is active, and every
`available` module whose components all report installed is active; the rest
is `not_installed`. The seed waits until every component owner answered; an
owner that answers `UNIMPLEMENTED` (built before the component calls, or not
configured) counts as installed, and after `[modules] seed_wait_s` (300 s)
an owner that still cannot be reached counts as installed too. Until the
seed, `settled` is false everywhere and the module routes that change state
answer 503; consumers keep their last known state.

### Jobs

`module_job` is the persistent queue: `kind` (`install`, `uninstall`,
`purge`), `state`, `reason`, `owner` (the service a failure names),
`bytes_done`/`bytes_total`, `requested_by`, `state_since`. One job runs at a
time on the engine's own worker thread (`ModuleEngine`, a `jthread`
registered as the `settings-modules` drain, woken by every request and
otherwise every `poll_interval_ms`); the owner RPCs and the synchronous
SQLite writes run there or, for a request, inside a `BlockingTask` on the
light lane, never on the event loop. Every state change of the database runs
in one `BEGIN IMMEDIATE` transaction (job, lifecycle and audit together); a
failed write rolls back and reloads the in-memory state.

- Install: `queued → checking → downloading → verifying → activating →
  health_check → done`. `checking` reads the owners fresh: an unreachable
  owner keeps the job there with reason `owner_unreachable`; a hardware
  verdict of `insufficient` fails it (`hardware_insufficient`); a
  provisioned component that is not on disk fails it (`host_only`); if
  everything is installed it skips to `verifying`, otherwise it asks each
  owner to `InstallComponent` the missing downloads. `downloading` polls
  `ComponentStates`: progress is the bytes present over the module's bytes,
  stored as `max(previous, present)` so it never decreases; a component in
  `failed` cancels the module's downloads and fails the job with the owner's
  own reason (`disk_full`, `network`, `source_unavailable`,
  `checksum_mismatch`); a component that went missing again is asked again.
  `verifying` confirms every component installed (the owner verifies the
  SHA-256 before it renames). `activating` makes the module `active` in the
  same transaction that moves the job on (never half enabled) and publishes
  the enabled set; `health_check` waits for every component's `ready` and
  after `[modules] health_timeout_s` (180 s) rolls the module back to
  `disabled` and fails the job with `health_check_failed`.
- A module's requirements are queued first (deps-first order); a job whose
  requirement ended without enabling it fails with `dependency_failed`.
- Pause (`queued` to `verifying`) stops the owners' fetches with
  `CancelComponent` (the partial files stay for the resume); resume queues
  again; cancel ends the job. `activating` and `health_check` refuse both
  (409 `CONFLICT`).
- Boot resumes every job that was running: it goes back to `queued` with its
  counters, so progress continues from what the owners already hold; a paused
  job stays paused and its fetches are stopped again.
- Uninstall (`kind` `uninstall`, keep data) and purge (`kind` `purge`):
  `queued → removing (→ purging) → done`. Asking for it disables an active
  module at once, in the same transaction that creates the job. `removing`
  asks each owner to `RemoveComponent` the module's downloaded components
  that no other active module uses (provisioned files are the host's and
  stay); an owner that answers `UNIMPLEMENTED` fails the job with
  `remove_unsupported`, one that cannot be reached with `owner_unreachable`,
  one whose files remain with `remove_failed`, each naming the `owner`. Keep
  data ends `uninstalled_data_kept` (or stays `not_installed`). `purging`
  asks every `dataOwners` service that has not purged yet to
  `PurgeModuleData`; each confirmation is a `module_purge` row, so a retry
  (a new purge request) resumes from the owners left; `UNIMPLEMENTED` fails
  with `purge_unsupported`, a refusal with `purge_failed`. When every owner
  confirmed, the module is `not_installed`, `data_purged_at` is stamped and
  the purge rows are cleared.
- A purge needs the Owner's guard PIN when one is set. Settings asks argus-guard
  over the settings wire (`VerifyOwnerPin`): no PIN or the right one goes on,
  a missing PIN is 403 `PIN_REQUIRED`, a wrong one 403 `PIN_INVALID`, too
  many 429 `PIN_LOCKED`, an unreachable guard 503. argus-guard implements it
  (its duress code answers `invalid` and raises the silent alert, never
  approving a purge: `services/guard/CONTEXT.md`); a guard that answers
  `UNIMPLEMENTED` (an older build) leaves the app's typed confirmation as the
  only check.

### Hardware check

`services/hardware-check.cc`, fed by `HardwareProbe::get()` and `statvfs` on
`[modules] models_dir` (in the deploy the models root bind-mounted read-only
at `/opt/argus/models`). RAM counts the minimum and recommended RAM of every
active module plus the target and what it requires; below the sum of minimums
is `insufficient` (`ram_below_minimum`), below the recommended sum `slow`
(`ram_below_recommended`). Free disk must hold the remaining download bytes
plus 10 % (`disk_insufficient`, insufficient); an unreadable disk is left out.
A missing `requiredCpu` feature is insufficient, a missing `recommendedCpu`
one slow (`cpu_feature_missing`), a recommended GPU without Vulkan slow
(`gpu_missing`). Install refuses `insufficient` with 409
`MODULE_HARDWARE_INSUFFICIENT`; `slow` is allowed and shown.

### HTTP contract

Filters `DeviceFilter → (ValidJsonFilter on POST) → JwtFilter → RoleFilter`;
`role_access::kModuleAccess` lets every role read `GET /modules` and keeps
the rest owner-only. Every POST takes a JSON object body (`{}` when it has
nothing to say). Names, summaries and getting-started titles come in the
`Accept-Language` language (`en…` gives English, anything else Spanish).

`GET /modules` → `{ "modules": [...] }` in catalog order. The Owner gets:

```json
{ "id": "surveillance", "name": "Vigilancia", "summary": "…",
  "kind": "available", "lifecycle": "not_installed", "enabled": false,
  "requires": ["core"], "sizeBytes": 578108270, "installedBytes": 9763086,
  "hasData": false, "dataPurgedAt": null,
  "hardware": { "verdict": "ok", "reasons": [], "minRamMb": 1536,
                "recommendedRamMb": 3072, "freeDiskMb": 120000 },
  "job": null,
  "gettingStarted": [ { "id": "add-camera", "title": "Agrega tu primera cámara",
                        "route": "/cameras" } ],
  "components": [ { "id": "detector", "owner": "camera", "source": "provisioned",
    "reachable": true, "reported": true, "state": "installed",
    "bytesPresent": 9763086, "bytesTotal": 9763086, "ready": true,
    "hostCommand": "services/camera/scripts/provision.sh", "reason": null } ] }
```

- `kind`: `core` | `available` | `coming_soon`. `lifecycle` as above.
  `dataPurgedAt`: unix ms or null. `freeDiskMb` is null when unreadable.
- `job`: the module's latest job unless it is `done`, else null:
  `{ "id", "kind": "install"|"uninstall"|"purge", "state", "progress" (0-1),
  "bytesDone", "bytesTotal", "bytesPerSecond", "etaSeconds" (null when
  unknown), "reason" (null or a code), "owner" (null or the service a
  failure names) }`. States: `queued`, `checking`, `downloading`,
  `verifying`, `activating`, `health_check`, `removing`, `purging`, `done`,
  `paused`, `failed`, `cancelled`.
- `components` (an addition to the plan's JSON): the app shows a provisioned
  component's `hostCommand`; `reported` is false for an owner that does not
  implement the component calls yet, `reachable` false for one that did not
  answer.
- Every other role gets `{ "id", "name", "enabled", "lifecycle",
  "dataPurgedAt" }` only, so their devices also drop purged data.

| Route | Answer |
|---|---|
| `POST /modules/{id}/install` | 202 with the job. 404 unknown; 409 `MODULE_COMING_SOON`, `MODULE_HARDWARE_INSUFFICIENT`, `MODULE_JOB_RUNNING` (this module already has an unfinished job), `CONFLICT` (already active); 503 before the seed. |
| `POST /modules/{id}/pause`, `/resume`, `/cancel` | 200 with the job; 404 `NOT_FOUND` when the module has no unfinished job; 409 `CONFLICT` while activating or in health check. |
| `POST /modules/{id}/disable` | 200 with the module; 409 `MODULE_CORE`, `MODULE_REQUIRED_BY`, `MODULE_JOB_RUNNING`. |
| `POST /modules/{id}/uninstall` | body `{ "keepData": true (default) | false, "pin": "digits" }`; 202 with the job; 409 `MODULE_CORE`, `MODULE_REQUIRED_BY` (message "Required by <id>"), `MODULE_JOB_RUNNING`; 403/429 PIN codes and 503 as above; 422 for a non-boolean `keepData` or a non-numeric or over-32-character `pin`. |
| `GET /modules/{id}/data` | `{ "owners": [ { "owner", "reachable", "reported", "items": [ { "kind", "count" } ], "bytes" } ] }`, one entry per `dataOwners` service, read live in parallel. |

### Events on `argus.settings.v1.module`

The producer creates the stream at boot (`ARGUS_SETTINGS_MODULE`, subjects
`[argus.settings.v1.module]`, max age 24 h, duplicate window 120 s;
`nats_subject::kSettingsModule`/`kSettingsModuleStream`); consumers may
create-or-bind it with that exact configuration. Every message has a
`Nats-Msg-Id` `<bootMs>-<sequence>`. Two kinds, both JSON objects:

```json
{ "kind": "enabled", "version": 42, "settled": true, "at": 1790000000000,
  "modules": [ { "id": "surveillance", "enabled": true, "lifecycle": "active",
                 "dataPurgedAt": null, "roles": ["guard"], "kind": "available",
                 "name": { "es": "Vigilancia", "en": "Surveillance" },
                 "summary": { "es": "...", "en": "..." },
                 "intro": { "es": { "what": "...", "examples": ["...", "..."] },
                            "en": { ... } } },
               ... every catalog module ... ] }

{ "kind": "module", "version": 42, "settled": true, "at": 1790000000000,
  "module": { ...the Owner's module JSON above, names in Spanish... } }
```

- `enabled` is published on every lifecycle change and once per boot after
  the seed (retried each engine pass until NATS takes it), so a
  `deliverAll` consumer learns the latest set. A consumer drops a message
  whose `version` is lower than the last it applied and ignores
  `settled: false`. argus-sync fans it out as `ModuleUpdate` to every socket.
- `module` goes to the Owner's room (`ModuleUpdate`, info = the bare module).
  Throttle per job: a state change always goes out; a progress-only change
  only when at least a second passed and progress grew by at least 1 % since
  the job's last frame. The frame of a finished job carries that job.

### The action journal: who did what to a module

`module_audit` already records every lifecycle action with the user who asked
for it; the Owner's activity history (`GET /sync/activity`,
`services/sync/CONTEXT.md`) reads it from `user_action_log`, so each row is
also published as a `UserActionEvent` on `argus.settings.v1.user-action`
(stream `ARGUS_SETTINGS_ACTION`, 7 days, 120 s duplicate window). `ModuleJournal`
(`services/module-journal.*`, its own thread and its own SQLite connection,
because the engine's synchronous `BEGIN IMMEDIATE` transactions share one
connection and a statement from another thread would run inside them) relays
`module_audit` rows past the cursor kept in `module_journal`
(`published_through`, one row), in id order, one `Nats-Msg-Id`
`settings-action:<audit id>` each. The cursor advances only after the broker
accepted the entry, so a down broker holds the journal and the next pass
resumes at the same entry; a replay is absorbed by the message id and by
argus-sync's unique `user_action_log.msg_id`. The first journal of an existing
database starts at the last audit row (history is not invented into the
activity feed); on a fresh database it starts at zero, so the adoption rows
are journaled too.

An event is `table_name: "module"`, `record_id: 0`, `module: <module id>`,
`user_id: <who asked, 0 for the engine itself>`, an action (`adopted` and
`install_requested` create; `uninstall_requested`, `removed` and `purged`
delete; everything else updates) and `new_data: { event: <audit action>,
lifecycle?: <lifecycle after, for enabled, disabled, rolled_back and purged>,
detail?: <audit detail>, at: <unix seconds> }`. Sync stamps `created_at` when
it writes the row, as it does for identity's and auth's actions.

### Wire

- argus-settings calls each owner through `argus::clients::settings`
  (`SettingsClient::componentStates`, `installComponent`,
  `cancelComponent`, `removeComponent`, `moduleDataSummary`,
  `purgeModuleData`, `verifyOwnerPin`), on the owner's existing settings
  credential and the update deadline. Each answers `std::nullopt` when the
  owner says `UNIMPLEMENTED`.
- argus-settings serves `argus.settings.v1.Modules/ModuleStates` on
  `[rpc] address` (127.0.0.1:7047 native, 0.0.0.0:7047 deploy) for the
  services' boot read: `{ modules: [{ id, enabled, lifecycle,
  data_purged_at, roles, name_es, name_en, summary_es, summary_en,
  intro_es, intro_en }], version, settled }`, and `OwnerCatalog` (the Owner's
  whole module list as one JSON array, for argus-sync's context). Callers present their own
  credential from `[rpc.callers]` (auth, camera, guard, identity,
  notification, productivity, sync, voice, llm; `ensure_fleet_callers` pairs
  them with the caller's `[modules] credential`); the gate is open while none
  is paired. Before the seed, or with no catalog, it answers `settled: false`.

### Data owners

`dataOwners` in `modules.json` run in order and each deletes only its own data
through `ModuleDataSummary`/`PurgeModuleData`: `surveillance` is camera,
guard, identity and sync; `productivity` is productivity and sync. Camera and
guard are settings owners already; identity, productivity and sync are
**data-only owners**: `SettingsConfig::resolveDataOwners` reads
`[owners.identity|productivity|sync] target/credential`, they join the
component owners the engine calls, and the settings gateway never lists them
(their catalogs are empty). Each serves the calls on its existing gRPC
listener (identity 7040, productivity 7037, sync 7041) behind its own
`settings` caller credential, paired by `ensure_settings_owners`. sync is last
on purpose: it deletes the audit history of the module's tables after the
owners deleted the rows. What each owner deletes, keeps and reports is in its
CONTEXT.md ("summary and purge"). An owner whose data host throws answers
`UNAVAILABLE` (the contract catches it), which the engine treats as
unreachable and retries.

The callers of the enabled set (`[modules] target`/`credential`) are camera,
guard, identity and productivity, the services that install the module gate;
their templates carry `target = "127.0.0.1:7047"` (deploy
`argus-settings:7047`) and `ensure_fleet_callers` pairs the credential with
`[rpc.callers] <caller>` here. `ModuleStates` answers `enabled` only for an
`active` module, with its `lifecycle` on every entry
(`settings-module-engine-test`, "the enabled set answers enabled only for an
active module").

## Module tools over MCP (2026-10, the context plan)

`feature/mcp/` serves the voice surface of the module manager to argus-llm
(`argus.mcp.v1.Mcp/Rpc` on the modules RPC listener, 7047, for the caller `llm`
only, reusing the listener's `FleetCallerGate` and the `[rpc.callers] llm`
credential the module-state reads already use; unpaired, the tools are not
served). The tools sit behind a `ModuleDesk` port; `EngineModuleDesk` adapts
the `ModuleEngine` to it and the tests drive a fake desk.

| Tool | Capability | Behaviour |
|---|---|---|
| `modules.list`, `modules.explain {module}` | `modules.read` (every role) | what each module is, its state ("instalándose 42%"), its intro and examples, in the user's language |
| `modules.request {module}` | `modules.request` (non-Owner) | asks the household Owner to turn a module on; an Owner cannot ask itself; the route that records the request and notifies the Owner is the module-effects work (`POST /modules/{id}/request`), so until it exists the desk answers unavailable and the tool says so |
| `modules.enable {module}` | `modules.manage` (Owner) | starts the install; the answer says it started, is already on, is coming soon, does not fit this hardware or has a job running; argus-llm only calls it on the user's spoken yes |
| `modules.disable {module, confirmation?}` | `modules.manage` (Owner), destructive | first call: the impact preview in the user's words (what stops, who is affected, invitations that would be revoked, what keeps running, what stays) and a one-use token; second call with the token: turns it off. Nothing is deleted |
| `modules.open_purge_screen {module}` | `modules.manage` (Owner) | emits `app.open {screen: "modules", module}`; deleting a module's data is never done by voice |

The preview speaks `ModuleImpact::keepsRunning`, the localized lines the
impact carries for what a module change does not touch, most importantly that
a raised panic or duress alert goes on until someone answers it; the tool never
writes that sentence itself. Until the impact preview of the module-effects work
is reachable from here, `EngineModuleDesk::impact` answers a known module
"cannot be previewed from here, turn it off from the app" (code
`impact_unavailable`), so `modules.disable` refuses and issues no token rather
than turning a module off with an invented summary.

`tests/unit/settings-mcp-test.cc` pins every tool per role and language, the
one-use token (another user's, a spent one, another module's), the refusal
paths and the spoken `keepsRunning`; `settings-engine-desk-test.cc` runs the
adapter over a real `ModuleEngine` (states, enable outcomes, disable refusals in
both languages, the unavailable impact and request, no engine).
