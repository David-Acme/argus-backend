# Provisioning and models

## setup.sh

`scripts/setup.sh [dev|prod]` is the native entry point. It:

1. installs distro build dependencies;
2. installs/validates Conan and the C++20 profile;
3. initialises the `third_party` submodules;
4. runs each owner's `scripts/provision.sh`;
5. creates per-project 0600 `config.toml` files from their templates;
6. generates the local PKI and the hardware profile;
7. delegates the build to `build-all.sh`.

Flags: `--no-build` / `SKIP_BUILD=1` (install only) and `camera` (camera
artifacts only).

## Model provisioning per owner

| Owner script | Artifacts |
|---|---|
| `services/argus-tts/scripts/provision.sh` | Supertonic 3 |
| `services/argus-stt/scripts/provision.sh` | sherpa-onnx models |
| `services/argus-llm/scripts/provision.sh` | LFM2.5-1.2B-Instruct QAD |
| `services/argus-vlm/scripts/provision.sh` | LFM2.5-VL-450M GGUF + mmproj |
| `services/argus-voice/scripts/provision.sh` | Silero VAD |
| `services/argus-camera/scripts/provision.sh` | YOLO26n export + go2rtc |
| `packages/argus-memory/scripts/provision.sh` | e5-small embeddings, NuExtract |
| `packages/argus-identity/scripts/provision.sh` | RetinaFace + MobileFaceNet |

Models live in the shared `models/` tree and are never copied into projects or
the image. Each download uses a `.part` file, SHA-256 verification and an
atomic move; mismatched files are replaced.

## Local PKI and hardware profile

- `setup_certs()` creates the instance CA and server certificate under
  `certs/` (gitignored, 0600 key material).
- `scripts/detect-hardware.sh` writes `scripts/.hw-profile` (CPU, RAM, GPU,
  Vulkan/CUDA capability) consumed by the tier logic; see
  [hardware-tiers.md](hardware-tiers.md).
