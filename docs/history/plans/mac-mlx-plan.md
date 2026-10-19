# Mac host and MLX plan, 2026-10-19

The owner expects to run Argus on a Mac Mini (Apple Silicon, 24 GB unified
memory, 512 GB SSD) and wants MLX models supported there. This plan records
what was measured or sourced on 2026-10-19 and orders the work into units.
The build-speed unit (`build-speed-plan.md`) already landed the build-system
part that costs nothing on Linux; everything below is still to do.

## 1. What already holds after the build-speed unit

- ggml is built once per machine by `cmake/argus-vendored.cmake`; on Apple
  `ARGUS_GGML_GPU=AUTO` selects Metal with the embedded library, OpenMP off
  and Accelerate on, and never Vulkan (a LunarG SDK on the Mac no longer
  turns a second backend on).
- ncnn is built once per machine; on Apple its Vulkan and OpenMP are off by
  default (`ARGUS_NCNN_GPU`), so neither MoltenVK nor Homebrew `libomp` is
  needed. The face and YOLO models are 2–9 MB and run on NEON.
- mold is never selected on Apple; the service rpath is `@loader_path` there
  and `$ORIGIN` elsewhere (`ARGUS_RPATH_ORIGIN`).
- `build-all.sh` reads free memory through `vm_stat` and the core count
  through `sysctl` when `/proc` is missing. Its parallel project mode needs
  bash ≥ 5.1 (Homebrew `bash`) and ninja ≥ 1.13; with the system bash 3.2 it
  falls back to one project at a time.

## 2. What blocks a native macOS build today

There is no `__APPLE__` or `__linux__` guard in first-party code.

| Kind | Production sites |
|---|---|
| epoll / eventfd (the tunnel's poll loop) | `services/tunnel/src/net/poll-loop.{hxx,cc}`, `link/tls/tls-channel.cc`, `net/tcp-listener.cc`, `net/tcp-peer.cc` |
| `SOCK_NONBLOCK`/`SOCK_CLOEXEC` in `socket()` | `tunnel/src/link/tls/tls-channel.cc:97`, `tunnel/src/net/tcp-listener.cc:22`, `tunnel/src/net/tcp-peer.cc:68`, `camera/src/shared/services/stream/upstream-http.cc:67`, `packages/lib/net/src/net/loopback-socket.cc:123` |
| `accept4` | `tunnel/src/net/tcp-listener.cc:63-64` |
| `pipe2` | `camera/src/feature/actions/audio-capture.cc:136` |
| `cpu_set_t`/`sched_getaffinity` (inline in a header, so every lib/runtime consumer) | `packages/lib/runtime/src/runtime/cpu-limits.hxx:225-229` |

Plus about 17 test sites with the same calls. `MSG_NOSIGNAL`, `O_CLOEXEC`,
`getifaddrs` and `posix_spawn` exist on macOS and are not blockers.

Runtime degradations (compile, wrong answer): `/proc/meminfo` and
`/sys/devices/system/cpu` in `hardware-profile.cc` (available RAM reads 0,
physical cores read as logical), `/proc/self/statm` in
`services/llm/src/feature/llm/services/turn/onnx-session.cc:26`, and
`llmGpuLayers()`/`vlmGpuLayers()` returning 0 without Vulkan
(`hardware-profile.cc:289-301`), so a Metal build would still run on CPU.

Other: the LiveKit SDK pin covers Linux only (v1.12.0 publishes
`livekit-sdk-macos-arm64-1.12.0.tar.gz`); `setup.sh` has no Homebrew branch
and uses GNU `sed -i`; `scripts/lib/common.sh` needs `flock`; 79 uses of
`std::jthread` and 154 of `stop_token` need a libc++ where they are not
experimental (LLVM ≥ 20; Homebrew LLVM if Apple clang lags).

## 3. Containers on the Mac

Docker Desktop runs a Linux VM with no Metal; in-container inference is
CPU-only arm64, `/dev/dri` does not exist, host networking is layer-4 only
and LAN multicast does not reach containers, so the per-service mDNS
announcements and LiveKit's UDP break. On the Mac the services run natively
under launchd; NATS and RustFS may stay in containers. The container-only
deployment rule (AGENTS.md §17b) needs a stated Mac exception, which is an
owner decision.

## 4. MLX

- MLX (MIT) has C++, C and Swift APIs, but no maintained C++ LLM/VLM runner:
  embedding it means rewriting LFM2's hybrid conv/attention, tokenizer,
  sampling and the LFM2-VL projector. Not worth it.
- `mlx_lm.server` and `mlx_vlm.server` (MIT, Python) serve OpenAI-style HTTP;
  mlx-lm's own docs call its server not production grade.
- LiquidAI publishes `LFM2.5-1.2B-Instruct-MLX-{4,5,6,8bit,bf16}` and
  `LFM2.5-VL-450M-MLX-{4,5,6,8bit,bf16}`; mlx-vlm has `lfm2_vl`. None is the
  QAD Q4_0 Argus ships, so quality must be re-measured.
- Sourced speed: MLX beats llama.cpp Metal by single digits at 1–3B and by
  more only at 14B+. For 0.45–1.2B models the gain is small and not measured
  on LFM; prompt processing and time to first token dominate a voice turn.
- Memory: the whole model set is about 6–8 GB resident, so 24 GB leaves more
  than 14 GB free with or without an MLX sidecar.

## 5. Units, in order

| Unit | Content | Needs the Mac |
|---|---|---|
| M1 portability: runtime | `cpu-limits.hxx` affinity behind `__linux__`; Darwin probes in `hardware-profile.cc` (`sysctl hw.perflevel0.physicalcpu`, `host_statistics64`, Metal present); `llmGpuLayers`/`vlmGpuLayers` answer "all" on Apple Silicon; `gpu_layers` defaults become `auto` | build only |
| M2 portability: sockets | one `socket` + `fcntl(FD_CLOEXEC, O_NONBLOCK)` helper in `lib/net`, `accept` + `fcntl`, `pipe` + `fcntl`; the `/proc/self/statm` reader gets a `task_info` path | build only |
| M3 tunnel | a kqueue (or `poll()`) backend behind `PollLoop` | build + tunnel tests |
| M4 scripts and LiveKit | Homebrew branch in `setup.sh`, portable `sed`, a `flock` stand-in, LiveKit macOS archive and SHA pin, `.dylib` names | yes |
| M5 native run profile | launchd plists for the native services; compose limited to NATS and RustFS; the owner's Mac exception to §17b | yes |
| M6 measure llama.cpp Metal | the existing eval harnesses and TTFT/latency on the Mac: the baseline MLX must beat | yes |
| M7 MLX engine, optional | `[llm] engine = "llama" \| "mlx"` and the same for `[vision]`; an `infra/mlx/` adapter talks to a loopback `mlx_lm.server`/`mlx_vlm.server` through lib/http's Drogon client, `/v1/completions` with the prompt Argus already builds so ChatML and sampling stay identical; pinned venv (`mlx-vlm==0.3.9` per the model card), `LiquidAI/*-MLX-8bit` SHA-verified like the GGUF; supervised sidecar with health checks and fallback to llama.cpp Metal | yes |
| M8 CoreML, optional | `[onnx] provider = "cpu" \| "coreml"` read by the ORT session builders and sherpa's `provider`; needs an onnxruntime with CoreML (the Conan recipe has no option for it); enabled per model only where measured (fp32 Supertonic and e5 likely, INT8 models likely not) | yes |

M7 ships only after the voice-quality gates and a TTFT comparison pass against
M6 on the same Mac.

## 6. Sources

- MLX: https://github.com/ml-explore/mlx, https://github.com/ml-explore/mlx-c,
  https://github.com/ml-explore/mlx-lm/blob/main/mlx_lm/SERVER.md,
  https://github.com/Blaizzy/mlx-vlm
- Models: https://huggingface.co/LiquidAI/LFM2.5-VL-450M-MLX-8bit,
  https://huggingface.co/api/models?author=LiquidAI&search=MLX
- Speed: https://github.com/ggml-org/llama.cpp/discussions/4167,
  https://www.local-llm.net/compare/llama-cpp-vs-mlx/,
  https://arxiv.org/abs/2511.05502v1
- llama.cpp build: https://github.com/ggml-org/llama.cpp/blob/master/docs/build.md
- CoreML EP: https://onnxruntime.ai/docs/execution-providers/CoreML-ExecutionProvider.html
- Docker on Mac: https://docs.docker.com/desktop/features/gpu/,
  https://docs.docker.com/engine/network/drivers/host/
- Darwin `MSG_NOSIGNAL`: https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/socket.h
- LiveKit SDK release: https://github.com/livekit/client-sdk-cpp/releases/tag/v1.12.0
