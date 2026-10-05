# Provisioning and models

## The privacy notice comes first

Both `setup.sh` and `provision-host.sh` print the Argus privacy notice and the
pre-beta terms before they touch anything, and stop unless the owner accepts
them for the household (David, 2026-10-04: consent before installing or
configuring). The texts are `scripts/privacy/notice.{es,en}.md`, rendered with
the country table `scripts/privacy/jurisdictions.tsv` (Peru first: Ley N°
29733, D.S. N° 016-2024-JUS and the videovigilancia Directive N°
01-2020-JUS/DGTAIPD with its 30/60/120-day periods); a new country is one row,
never a code change. `scripts/lib/privacy.sh` holds the flow.

- Interactive runs ask the owner to type `ACEPTO` (`ACCEPT` in English);
  anything else cancels, and nothing is written.
- Non-interactive runs stop with exit code 3 unless `--accept-privacy-notice`
  (or `ARGUS_ACCEPT_PRIVACY_NOTICE=1`) is given. `-y` never accepts it: `-y`
  answers package managers, not the owner.
- The acceptance is recorded in `<data dir>/privacy/host-consent.json` (0600,
  folder 0700): notice and terms version, jurisdiction, language, UTC time,
  `user@host`, the method (`interactive` or `flag`), the script and the
  SHA-256 of the rendered text; every acceptance and withdrawal is appended to
  `host-consent.log`. A run with the current version recorded does not ask
  again; a new `PRIVACY_NOTICE_VERSION` asks once more.
- Recurring-visitor recognition (STRANGERS) has its own acknowledgement
  (`scripts/privacy/visitors.{es,en}.md`, `--accept-visitor-notice`, offered
  after the main notice and skipped by default). It is only recorded here:
  the feature stays off until the owner enables it in the app, which asks for
  the acknowledgement again.
- `--show-privacy-notice` prints the notice; `--withdraw-privacy-consent`
  removes the record (logged) and prints how to stop Argus and erase its data.
- Each person's own choices (presence, faces at cameras, voice learning,
  camera audio) are made in the app and stored by identity
  (`services/identity/CONTEXT.md`, "Privacy choices").
- `scripts/privacy-consent-test.sh` pins the refusal, the flag, the record's
  fields and modes, the no-repeat, the visitor acknowledgement, the withdrawal
  and the typed answer.

The notice is not legal advice; a lawyer should review it before any
commercial release (Peru's consumer code may limit how far the "as is"
disclaimer applies).

## setup.sh

`scripts/setup.sh [dev|prod]` is the native entry point. It:

1. installs distro build dependencies;
2. installs/validates Conan and the C++20 profile;
3. initialises the `third_party` submodules;
4. runs each owner's `scripts/provision.sh`;
5. creates per-project 0600 `config.toml` files from their templates;
6. generates the local PKI and the hardware profile;
7. delegates the build to `build-all.sh`.

Flags: `--no-build` / `SKIP_BUILD=1` (install only), `camera` (camera
artifacts only), `--no-docker` (skip the Docker check) and `-y`/`--yes`
(non-interactive package installs). The privacy flags are described above.

## Deployment host provisioning

`scripts/provision-host.sh` prepares a Linux or macOS deployment host without
compiling: it installs/validates Docker + Compose v2, creates the external
data tree and `argus-deploy/.env`, generates the instance PKI and fills the
per-service deploy configs with unique shared secrets. `--with-models`
downloads the weights through the same owner `provision.sh` scripts, and
`--start` builds and starts the stack. Re-running it is safe: existing certs,
secrets and databases are reused.

## Model provisioning per owner

| Owner script | Artifacts |
|---|---|
| `services/tts/scripts/provision.sh` | Supertonic 3 |
| `services/stt/scripts/provision.sh` | sherpa-onnx models |
| `services/llm/scripts/provision.sh` | LFM2.5-1.2B-Instruct QAD, e5-small embeddings, NuExtract |
| `services/vlm/scripts/provision.sh` | LFM2.5-VL-450M GGUF + mmproj |
| `services/voice/scripts/provision.sh` | Silero VAD |
| `services/camera/scripts/provision.sh` | YOLO26n export + go2rtc |
| `services/identity/scripts/provision.sh` | RetinaFace + MobileFaceNet |

Models live in the shared `models/` tree and are never copied into projects or
the image. Each download uses a `.part` file, SHA-256 verification and an
atomic move; mismatched files are replaced.

## Local PKI and hardware profile

- `setup_certs()` creates the instance CA and server certificate under
  `certs/` (gitignored, 0600 key material).
- `scripts/detect-hardware.sh` writes `scripts/.hw-profile` (CPU, RAM, GPU,
  Vulkan/CUDA capability) consumed by the tier logic; see
  [hardware-tiers.md](hardware-tiers.md).
