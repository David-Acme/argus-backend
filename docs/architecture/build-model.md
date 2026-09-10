# Build model

## Standalone projects

The repository root has no CMake project. Nineteen owner projects build
independently, each with its own Conan graph and `dev`/`prod` presets:

```
packages/argus-common      packages/argus-identity   services/argus-gateway
packages/argus-contracts   packages/argus-sync       services/argus-camera
packages/argus-cert        packages/argus-memory     services/argus-productivity
packages/argus-socket      packages/argus-intent     services/argus-notification
packages/argus-sqlite                                services/argus-tts
                                                     services/argus-stt
                                                     services/argus-vlm
                                                     services/argus-llm
                                                     services/argus-voice
                                                     services/argus-tunnel
```

Each project carries `CMakeLists.txt`, `conanfile.txt` and
`CMakePresets.json` (Ninja, Debug under `build/dev`, Release under
`build/prod`). `scripts/build-all.sh` drives all of them; see
[build-and-test.md](../operations/build-and-test.md).

## Third-party map

Sources stay shared in `third_party/` and every project compiles its own copy
at build time. Pins come from `.gitmodules` and `git submodule status`.

| Dependency | Type | Pin | Compiled by |
|---|---|---|---|
| `sqlite-vec` | vendored | v0.1.10-alpha.4 | gateway, camera, productivity, notification, cert, sync, identity, memory, sqlite, llm |
| `ncnn` | submodule | `4c1110c9` | gateway, camera, productivity, notification, cert, sync, identity |
| `llama.cpp` | submodule | `31558dbb` | memory, llm, vlm |
| `sherpa-onnx` | submodule | `dc130227` | stt |
| `fastText` | submodule | `1142dc4` | intent |
| `stb` | vendored | `stb_image.h` | identity, sync, voice |
| `go2rtc` | downloaded binary | release artifact | camera provisioning |

## Shared model artifacts

Model weights are runtime artifacts under `models/` (gitignored); each owner
provisions and resolves its own subfolder, and the image never bakes them.
The intent classifier is the one tracked artifact (`models/intent/intent.bin`)
with a configure-time SHA-256 pin.

## Wrappers and includes

- `cmake/argus-module.cmake` is the shared build substrate (`argus_module()`,
  `argus_sdk_module()`, `argus_service()`, protobuf/gRPC resolution). Every
  project includes it by relative path.
- Source include prefixes (`argus-*/src/...`) are preserved across the tree,
  so moving a project does not rewrite `#include` lines.
