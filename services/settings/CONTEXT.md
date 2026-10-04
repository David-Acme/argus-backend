# argus-settings

The single owner-facing HTTP surface of the app's Settings area. It owns no
data: every microservice that exposes tunable behaviour publishes its own
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
  and `llm.gpu_layers`: Vulkan offload measured 1.6–1.9× faster, but it needs
  `/dev/dri` in the container, a deploy decision. `vision.max_tokens`: the
  image encoder dominates (a one-word answer costs 60–75 % of a sentence).
  `vision.image_max_tokens`: below 256 the model stops reading text.
  `tts.quality`/steps only shape Supertonic, the fallback engine. Voice turn
  keys (VAD) are about the room, not the hardware. Guard, camera and
  notification keys are security and privacy: never in a profile.

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
- The GPU is reported, not used: no profile sets `gpu_layers`.
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
owner's caller slot (`[rpc.callers] settings` in tts, stt, vlm and llm;
`[grpc] caller_settings` in voice, and camera/notification when they adopt
it) and to `[owners.<owner>] credential` here; the target comes from the
owner's own gRPC listener. An owner with no caller slot stays unconfigured;
guard has no gRPC listener today. Existing secrets and targets are never
overwritten.

The native sandbox (`scripts/native-stack.sh`) empties the target of every
owner it does not boot, so the golden replay never depends on processes
outside the sandbox.
