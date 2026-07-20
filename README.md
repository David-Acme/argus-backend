# Argus Backend

**Argus** is a 100% local, modular and decoupled artificial-intelligence platform
for the home. Its starting point is an intelligent guard (computer-vision based
security), but it is designed to grow into a full local virtual assistant.

> Everything is processed locally. Cloud is only used for connectivity when the
> user is away from the LAN; it never processes data.

- **Language:** C++20
- **HTTP / WebSocket framework:** [Drogon](https://github.com/drogonframework/drogon)
- **Database:** SQLite (via Drogon's native async `DbClient`, no ORM, manual SQL)
- **Package manager:** Conan 2 (`conanfile.txt` + `CMakePresets.json`)

## Philosophy

- **Local first, private by default.** No video, images, audio or conversations
  leave the user's hardware unencrypted.
- **Rules before LLM.** Every event first goes through the rule engine. The LLM
  is invoked only when rules cannot resolve the situation. This keeps latency low,
  resource usage minimal and the system stable.
- **Provider abstraction.** Every capability is behind an interface
  (`ILLMProvider`, `IVisionProvider`, `IFaceProvider`, `ISTTProvider`,
  `ITTSProvider`, ...), so any model can be swapped without touching the rest.

## Architecture

The system is a pipeline of decoupled modules connected by interfaces:

```text
Camera
  ↓
Vision      (capture, preprocess, YOLO detection, ONNX Runtime)
  ↓
Tracking     (entities, not just boxes: id, history, velocity, zone, behavior)
  ↓
Behavior     (geometry-based: loitering, approaching, following, ...)
  ↓
Rules        (condition → action, no AI needed)
  ↓
Context      (builds a concise textual summary for the model)
  ↓
AI           (LLM orchestrator + tool calling)
  ↓
Automation   (lights, doors, IoT, automations — independent of the LLM)
```

The central abstraction is the **Entity**: a live object with state (track id,
face id, class, first/last seen, zone, velocity, direction, history, current
behavior, metadata). The whole backend works with `Entity`, never with raw
detections, so the detector/tracker can be replaced transparently.

## Module map

| Module            | Responsibility                                              | Key libs                          |
|-------------------|-------------------------------------------------------------|-----------------------------------|
| Vision            | Camera capture, preprocessing, detection                    | OpenCV, ONNX Runtime, YOLO (ONNX) |
| Tracking          | Maintain `Entity` state across frames                       | Eigen (geometry/Kalman)           |
| Face Recognition  | Crop → embedding → match                                    | InsightFace (submodule)           |
| Behavior Analyzer | Geometry-based behavior classification                      | OpenCV, Eigen                     |
| Rule Engine       | Event → condition → action                                  | (own IP)                          |
| Context Builder   | Summarise state into LLM-ready text                         | nlohmann_json                     |
| LLM               | Conversation, reasoning, tool calling                       | llama.cpp                         |
| Automation Engine | Control devices / automations                               | (own IP)                          |
| Memory            | SQLite (users, events, config, history, embeddings)         | Drogon `DbClient`                 |
| Blob storage      | Images / video / audio                                      | RustFS (planned)                  |

## Tech stack

Resolved via Conan 2 (`conanfile.txt`):

- `drogon/1.9.13` — HTTP/WebSocket server (+ built-in logging, used instead of spdlog)
- `jwt-cpp/0.7.2` + `nlohmann_json/3.11.3` — auth / JSON
- `libsodium/1.0.22` — crypto
- `llama-cpp/b6565` — local LLM inference
- `opencv/4.13.0` — computer vision (built **headless**: `with_protobuf`,
  `with_eigen`, `with_ffmpeg`, `with_wayland`, `with_gtk`, `with_vulkan` off)
- `onnxruntime/1.24.4` — ONNX model inference (CPU by default; CUDA opt-in)
- `eigen/5.0.1` — linear algebra (tracker / Kalman)
- `protobuf/6.33.x` — pulled transitively by onnxruntime

Bundled as git submodules under `third_party/` (built via `add_subdirectory`):

- `fastText` — text classification / intent detection (target `fasttext-static_pic`)
- `inspireface` (HyperInspire/InspireFace) — face recognition SDK, **opt-in** via
  `ARGUS_BUILD_INSPIREFACE=ON` (needs MNN/InspireCV cloned manually + model packs)

## Build

```bash
./scripts/setup-dev.sh            # dev profile, full build
./scripts/setup-prod.sh           # prod profile, full build
SKIP_BUILD=1 ./scripts/setup-dev.sh   # install deps only
```

The server listens on `0.0.0.0:7024`. Database file: `data/argus.db`
(outside `build/`, gitignored).

### Enabling InspireFace

```bash
cmake -S . -B build/dev -DARGUS_BUILD_INSPIREFACE=ON
# then clone MNN + InspireCV into third_party/inspireface/3rdparty/ and download models
```

## License

See [LICENSE](./LICENSE). Note: InsightFace/InspireFace **model packs** are
research/non-commercial only; commercial use requires a license from insightface.ai.
