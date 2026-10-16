# Bonsai 2 2B against the deployed LFM models, under equal conditions

Date: 2026-10-16. Requested by the owner after PrismML published the Bonsai 2 small family.

## Question

Is Ternary Bonsai 2 2B (PrismML, derived from Qwen3.5-2B, Apache 2.0, text and vision) better than the models Argus ships today? It is compared with:

- **LLM:** `LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf`, used by `services/llm`;
- **VLM:** `lfm2vl-25/lm-Q8_0.gguf` with `mmproj-F16.gguf`, LFM2.5-VL-450M, used by `services/vlm`.

## Bonsai 2 requirements, as applied

- **Runtime.** The PrismML llama.cpp fork, release `prism-b10777-e3de715`, the one the Bonsai-demo repository pins. Prebuilt `ubuntu-x64` (CPU) and `ubuntu-vulkan-x64` archives, both checked against the SHA-256 digests GitHub publishes.
  - Upstream llama.cpp rejects `PQ2_0`/`PTQ1_0`.
  - Upstream builds older than b11555 load `Q2_0` and produce gibberish: they lack the Hadamard activation transform.
- **Files.** Hugging Face `prism-ml/Ternary-Bonsai-2-2B-gguf`, revision `683f7be`, hashes verified:
  - `Ternary-Bonsai-2-2B-PQ2_0.gguf` (517 MB, the default format);
  - `Ternary-Bonsai-2-2B-mmproj-Q8_0.gguf` (365 MB, the recommended vision tower).
- **Thinking off.** Every request sends `chat_template_kwargs.enable_thinking=false`. Thinking is on by default and spends thousands of tokens on simple prompts.
- **Sampling, official non-thinking profile.** temperature 0.7, top-p 0.8, top-k 20, min-p 0, presence penalty 1.5, repeat penalty 1.0, penalty window 64.
- **Health check.** A Spanish prompt produced coherent text: the transform is active.

## Equal conditions

- **One binary for every model.** `llama-server` and `llama-bench` from the same fork release: the LFM files run on it unchanged.
- **One host.** AMD Ryzen 7 5825U (8 cores, 16 threads, AVX2), 30 GB RAM, Radeon iGPU (RADV). 8 threads, `-c 8192`, one slot, seed 42.
- **Sampling crossed.** Each LLM ran with its own recommended sampling *and* with the other's:
  - LFM production: temperature 0.3, top-k 20, top-p 0.8, repeat 1.1 over 64;
  - Bonsai's official non-thinking profile.
- **Identical inputs.** Every case gets the same messages for both models:
  - system: the production call prompt (`services/voice/tests/fixtures/call-prompt-{es,en}.txt`);
  - user: the utterance, followed by a «Nota de la app» line that states the turn kind, tool and facts.
- **The production pipeline is not in the loop.** The speech guard, act rendering and claim checks do not apply. The numbers compare the two models with each other; they are not a production gate.

## Conversation: `conversation-eval.py`, 127 cases of `conversation.jsonl`

| Model, sampling | False completion ↓ | Forbidden ↓ | Ungrounded ↓ | Mentions ↑ | Length ok ↑ | Questions ok ↑ | Turn p50 |
|---|---|---|---|---|---|---|---|
| LFM 1.2B, production | 0.126 | 0.024 | **0.063** | 0.551 | **0.984** | 0.731 | 2732 ms |
| LFM 1.2B, Bonsai profile | 0.102 | 0.008 | **0.063** | 0.606 | 0.969 | 0.769 | 2642 ms |
| Bonsai 2B, official profile | **0.024** | 0.008 | 0.110 | 0.614 | 0.866 | **0.962** | 1757 ms |
| Bonsai 2B, production sampling | **0.024** | 0.008 | 0.110 | 0.567 | 0.913 | 0.923 | 1652 ms |

### Findings

- **Bonsai rarely claims an action that did not happen.** With either sampling it does so 4–5× less often than LFM. LFM said, for example, "he desactivado el módulo de vigilancia" on a refused turn.
- **Bonsai is less fit for speech.** It:
  - sometimes echoes the «Nota de la app» into its reply;
  - adds emojis, Markdown and numbered lists;
  - garbles words ("gasífero", "a las nines");
  - adds more unsupported details.
- **Neither model meets the production conversation gates in this harness.** That is expected without the speech pipeline.
- **The shorter Bonsai turn latency is not explained.** Bonsai decodes slower per token (see Speed), yet its median turn was about 1 s shorter. A shorter tokenised prompt is a likely cause, but this run did not verify it.

## Vision: 8 infrared frames from the owner's living-room camera × 2 prompts

**Inputs.** Each frame was downscaled to 384 px, the production `max_input_px`. The two prompts:

- the production guard prompt from `guard-assessment.cc` ("Describe the person…");
- a Spanish scene prompt.

**Ground truth, from the owner.** Nobody is standing in any frame. He was sitting on the sofa, and the visible foot is him lying on it.

| | LFM2.5-VL-450M | Bonsai 2B |
|---|---|---|
| Correct descriptions (person present, posture, no invented people, animals or weapons) | **13/16** | 8/16 |
| Wall time per image, p50 | **1.8 s** | 4.9 s |
| Image prompt evaluation, p50 | **0.9 s** | 2.6 s |

### Findings

- **Bonsai:**
  - said "no person" while a person lay on the sofa, or described feet but no person;
  - invented a dog or pet three times.
- **LFM-VL:** described "lying on the couch" and "seated with the head bowed" correctly in most frames. Its one serious error: on frame `k1` it described a standing person holding "an object that resembles a knife". Bonsai also described a standing figure in `k1`, but no weapon.

**Follow-up for the guard prompt.** An invented weapon is a false alarm path in `argus-guard`. It is worth a look regardless of the model.

## Speed: `llama-bench`, pp512 and tg128, 3 repetitions

| Model | Size | CPU pp512 | CPU tg128 | Vulkan pp512 | Vulkan tg128 |
|---|---|---|---|---|---|
| LFM2.5-1.2B Q4_0 | 661 MiB | **166 tok/s** | **44.5 tok/s** | **387 tok/s** | **40.9 tok/s** |
| Bonsai 2 2B PQ2_0 | 483 MiB | 71 tok/s | 29.4 tok/s | 254 tok/s | 27.7 tok/s |
| LFM2.5-VL-450M LM Q8_0 | 359 MiB | 369 tok/s | 80.3 tok/s | 1285 tok/s | 85.9 tok/s |

On this x86 CPU, PrismML's published speed advantage, measured on an Apple M5 Max, does not appear. The iGPU speeds up prompt evaluation but not decode.

## Verdict

- **Vision:** keep LFM2.5-VL-450M. It is more accurate on real frames and about 2.7× faster.
- **Speaking LLM:** Bonsai 2B is a candidate, but not a drop-in.
  - For it: its low false-completion rate.
  - Against it: weaker speech form, more ungrounded detail, slower decode.
  - **Deciding test:** `call-faithfulness-eval` with the production speech pipeline. That needs an experimental runtime, see below.
- **Integration cost.** The vendored `third_party/llama.cpp` (b10305) cannot run Bonsai 2. Adopting it means one of:
  - the PrismML fork (`PQ2_0`);
  - upstream b11555 or newer with the `Q2_0` file.

  Either is a runtime change for both `services/llm` and `services/vlm`, and follows the model-artifact rules (AGENTS.md §13e).

## Reproduction

The working directory was `~/.cache/argus-bonsai/` (binaries, models, scripts, replies, reports). The text speaker reads the production call prompt, appends the app note and posts to the server's `/v1/chat/completions`. The vision script posts each frame as a base64 `image_url` with the same two prompts.

```
LD_LIBRARY_PATH=$BIN $BIN/llama-server -m <model.gguf> [--mmproj <mmproj.gguf>] -c 8192 -t 8 -tb 8 -np 1 --port <p> --no-webui
python3 -I services/llm/tests/eval/conversation-eval.py --cases services/llm/tests/fixtures/eval/conversation.jsonl \
    --gates services/llm/tests/eval/gates.json --speaker 'python3 -I speaker.py <p> services/voice/tests/fixtures <profile-json>'
LD_LIBRARY_PATH=$BIN $BIN/llama-bench -m <model.gguf> -t 8 -ngl {0|99} -p 512 -n 128 -r 3
```

## Sources

- [PrismML: Bonsai 2 family](https://prismml.com/news/bonsai-2-family)
- [Hugging Face: Ternary-Bonsai-2-2B-gguf](https://huggingface.co/prism-ml/Ternary-Bonsai-2-2B-gguf)
- [PrismML docs: troubleshooting](https://docs.prismml.com/resources/troubleshooting)
- [Bonsai-demo: BACKEND-SUPPORT.md and SMALL-MODELS.md](https://github.com/PrismML-Eng/Bonsai-demo)
- [PrismML llama.cpp fork releases](https://github.com/PrismML-Eng/llama.cpp/releases)
