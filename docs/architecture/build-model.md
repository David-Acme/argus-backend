# Build model

## Standalone projects

The repository root has no CMake project; its `conanfile.txt` is the tree's
single dependency manifest. Sixteen owner projects build independently, each
with its own `CMakeLists.txt` and binary directory, all configuring against the
one Conan graph `scripts/build-all.sh` resolves before the first of them:

```
packages/lib/cert          services/auth              services/guard
packages/lib/sqlite        services/identity          services/settings
                           services/sync              services/tts
                           services/camera            services/stt
                           services/productivity      services/vlm
                           services/notification      services/llm
                                                      services/voice
                                                      services/tunnel
```

Each project configures against the root graph's toolchain (Ninja, Debug under
`build/dev`, Release under `build/prod`). `scripts/build-all.sh` drives all of
them; see [build-and-test.md](../operations/build-and-test.md).

Every microservice also owns `services/<name>/Dockerfile`, built from
the repository root; packages are compiled into the service images and never
get an image of their own. See
[deployment-docker.md](../operations/deployment-docker.md).

## Third-party map

Sources stay shared in `third_party/` and every project compiles its own copy
at build time. Pins come from `.gitmodules` and `git submodule status`.

| Dependency | Type | Pin | Compiled by |
|---|---|---|---|
| `sqlite-vec` | vendored | v0.1.10-alpha.4 | auth, camera, productivity, notification, guard, sync, identity, sqlite, llm |
| `ncnn` | submodule | `a4d2ea1d` | camera, identity |
| `llama.cpp` | submodule | `31558dbb` | llm, vlm |
| `sherpa-onnx` | submodule | `546df6f9` | identity, stt |
| `fastText` | submodule | `1142dc4` | llm |
| `stb` | vendored | `stb_image.h` | identity |
| `go2rtc` | downloaded binary | release artifact | camera provisioning |

Every submodule pin is a commit of its upstream remote, so a fresh
`git clone --recurse-submodules` initialises each one from `.gitmodules`.
Nothing edits a submodule: it carries no local commit and `setup.sh` only
initialises it. What a consumer needs from one it sets from outside, before
its `add_subdirectory`: camera and identity force `NCNN_BUILD_TOOLS` and the
other `NCNN_*` switches as cache variables, and identity and stt give the
nlohmann json that sherpa-onnx fetches its own target name through json's
`NLOHMANN_JSON_TARGET_NAME`, so it never collides with Conan's
`nlohmann_json::nlohmann_json`.

## Shared model artifacts

Model weights are runtime artifacts under `models/` (gitignored); each owner
provisions and resolves its own subfolder, and the image never bakes them.
The intent classifier is the one tracked artifact (`models/intent/intent.bin`)
with a configure-time SHA-256 pin.

## Wrappers and includes

- `cmake/argus-module.cmake` is the shared build substrate — the group
  helpers (`argus_lib()`, `argus_contracts()`, `argus_clients()`), the
  ungrouped `argus_module()` and `argus_client_module()` for service-local and
  wire modules, `argus_service()`, and protobuf/gRPC resolution. Every project
  includes it by relative path.
- Source include prefixes (`argus-*/src/...`) are preserved across the tree,
  so moving a project does not rewrite `#include` lines.
