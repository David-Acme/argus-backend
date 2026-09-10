# Project timeline

Chronological trace of the work. Each entry names its milestone; the full
narrative lives in [project-log.md](project-log.md), and the focused
verification records live in [reports/](reports/).

| Period | Arc | Milestones |
|---|---|---|
| 2026-06 → 2026-08 | Foundations | C++20 backend on Drogon + SQLite, JWT dual secrets, face auth, Tapo camera integration, LLM/VLM/STT/TTS, `/sync` engine and audit. |
| 2026-08-08/09 | Real-time and stability | fMP4 over `/sync` with credit flow control, stateful audio resampler, VAD turn-taking, talk-channel fixes, conversational tuning. |
| 2026-08-10/20 | Memory redesign | SQLite semantic graph after the Kuetz gate failed, deferred extraction, vocabulary as static constants, tool-calling measurement. |
| 2026-08-22/23 | People and sync/audit | Invitations, role-scoped sync, portrait capabilities, audit resync, QAD model artifact. |
| 2026-09-02/03 | Camera consolidation | Face-detector decode fix, object detector, Tapo alarm read-modify-write, talk digest fix. |
| 2026-09-04 | F1 — gateway cutover | Public TLS listener in `argus-gateway`, legacy on an internal listener, NATS bus foundation, golden `/sync` fixtures. |
| 2026-09-05 | F2 — camera cutover | `camera.db` migration, NATS camera changes, camera sync reads, media re-target. |
| 2026-09-06 | F4 — AI service extraction | tts, stt, vlm, llm and memory cut over to their own processes over frozen internal wires. |
| 2026-09-07/08 | F6 — legacy retired | Voice cutover, camera cross-DB decoupling, legacy binary and `argus.db` removed, labs deleted. |
| 2026-09-08 | F7/F8-A/B — split and brain | Flat root becomes `packages/` and `services/`; memory becomes a package; the LLM tool loop is wired and re-measured. |
| 2026-09-09 | F8-C — build independence | Test-gate inversion, per-project Conan/presets, per-owner provisioning, the `build-all.sh` orchestrator. |
| 2026-09-09/10 | F9 — intent router | fastText returns as the router fast tier in front of tool calling, with a pinned artifact and eval fixtures. |
| 2026-09-10 | F8-C4-2 — root cut | Root CMake trio deleted; CI, `setup.sh` and the image build through `build-all.sh`; real `docker build` verified. |
| 2026-09-10 | F10 — hygiene | Curated English documentation tree, per-project ignores, dead root aggregator removed, one Docker image per microservice. |
