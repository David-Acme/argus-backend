# CONTEXT.md — Argus Backend project memory

> This file preserves the project's intent, decisions and state so context is
> never lost between sessions. Update it whenever a significant decision is made.

## What Argus is

- **Argus** is a 100% local AI platform for the home. Security ("intelligent
  guard") is the entry point; a local virtual assistant is a secondary capability.
- Philosophy: process everything locally, minimize resources, preserve privacy.
  Cloud only transports packets (tunnel), never processes data.
- AI is invoked **only when it adds value**: events → rule engine → if resolvable,
  execute; else → LLM. This keeps latency/cost low.
- Central abstraction: **Entity** (live object with state), not raw detections.
  The whole backend works with `Entity`.

## Hard constraints / decisions

- **C++20**, Drogon HTTP/WebSocket server. SQLite via Drogon async `DbClient`
  (NO ORM, manual SQL). DB file `data/argus.db`, `number_of_connections: 1`.
- **spdlog is NOT added** — Drogon already provides logging. Do not add spdlog.
- **Conan 2** for deps (`conanfile.txt` + `CMakePresets.json`). CMake presets:
  `dev` (Debug) and `prod` (Release), generator Ninja.
- **Git submodules** under `third_party/` for libs that change rarely and we want
  to control: `fastText`, `inspireface` (InspireFace). Built via `add_subdirectory`
  with `EXCLUDE_FROM_ALL`.
- Convention: Conventional Commits in English. Remote `git@github.com:David-Acme/argus-backend.git`,
  branch `main`.

## Dependency resolution notes (Conan conflicts, learned the hard way)

- `nlohmann_json` pinned to **3.11.3** — onnxruntime requires `<3.12`. (3.12.0 fails.)
- `opencv/4.13.0` built **headless**: `with_protobuf=False`, `with_eigen=False`,
  `with_ffmpeg=False`, `with_wayland=False`, `with_gtk=False`, `with_vulkan=False`.
  Reason: the ConanCenter prebuilt opencv pulls X11/Wayland system deps
  (`libxres`, etc.) that need sudo to install; headless avoids that and is leaner.
- `eigen/5.0.1` (onnxruntime requires `>=5.0.1`; opencv's eigen dropped via
  `with_eigen=False`).
- protobuf: unified via opencv `with_protobuf=False`; onnxruntime brings `6.33.x`.
- `onnxruntime/*:with_cuda=False` by default — CPU fallback. Enable CUDA only when
  GPU is available; never hard-require it.
- **conanfile.txt cannot resolve version conflicts** (no `override=True`/`force`).
  Conflict resolution is done by pinning versions + disabling the offending option
  in the consuming package. Keep this pattern.

## fastText CMake warnings (silenced)

- `third_party/CMakeLists.txt`: `CMAKE_POLICY_VERSION_MINIMUM` bumped to 3.10, and
  `CMAKE_CXX_STANDARD` restored to 20 after the fastText `add_subdirectory`.
- `scripts/setup-dev.sh` / `setup-prod.sh`: after submodule init, patch fastText's
  `set(CMAKE_CXX_STANDARD 17)` → `20` in place (submodule stays git-clean; re-applied
  each init). Do NOT edit the fastText submodule file directly.

## InspireFace (opt-in)

- Integrated behind `option(ARGUS_BUILD_INSPIREFACE OFF)` in `third_party/CMakeLists.txt`.
- OFF by default because InspireFace is heavy and pulls its own deps (MNN, InspireCV)
  that are **NOT git submodules** — they must be cloned manually into
  `third_party/inspireface/3rdparty/{MNN,InspireCV}` (see that repo's README), plus
  model packs downloaded. Also needs a license for commercial use (insightface.ai).
- `setup_submodules()` initialises nested submodules for InspireFace, but the MNN/
  InspireCV clones are still manual.

## Current build state

- Build is **green**: `cmake --preset dev` + `cmake --build --preset dev -j 8`
  succeeds; server starts on `0.0.0.0:7024`.
- fastText linked (static, `fasttext-static_pic`). InspireFace not linked (OFF).

## Planned architecture (not yet implemented)

`vision-core` engine (CameraManager, FramePreprocessor, Detector, Tracker,
FaceRecognizer, BehaviorAnalyzer, ZoneManager, EventManager, SnapshotManager,
ContextBuilder). Tracker: start with IoU+distance, **Kalman optional behind a flag**
(Eigen). Rule Engine / Context Builder / Behavior Analyzer are own IP. Communication:
WebSocket + TLS + own tunnel (LAN when local, tunnel when remote). Memory: SQLite +
RustFS for blobs. Profiles: light / balanced / advanced.

## Open questions / next steps

- Implement `README.md` + `CONTEXT.md` (done this session).
- Scaffold `vision-core` (separate lib or `src/vision-core/` — TBD when dev starts).
- Decide YOLO model export to ONNX behind `IVisionProvider`.
- RustFS integration for snapshots/video/audio blobs.
