# argus-llm-client

The argus-llm wire seen from the caller's side: the chat contract and the
HTTP client that speaks it.

## What this is

A module, not a service. argus-memory (formation and chat) and
argus-voice (session streaming) both reach argus-llm over HTTP, and
argus-llm implements the same contract — so the contract lives here
rather than in any one of the three.

## Layout

- `src/shared/services/llm/llm-service.hxx` — the chat DTOs
  (ChatMessage, ChatRequest, LlmPrefillStats) and the in-process engine's
  declaration.
- `src/shared/contracts/tool-contracts.hxx` — the tool-calling shapes
  that ride a chat request.
- `src/shared/services/llm/remote/llm-remote.{cc,hxx}` — the HTTP client.

## Rules

- Rule 25: the folder IS the module. One `argus_module(NAME llm-client ...)`.
- Include prefixes are load-bearing: consumers include
  `<shared/services/llm/llm-service.hxx>` and
  `<shared/services/llm/remote/llm-remote.hxx>`.
- No engine here. `llm-service.cc` (llama.cpp) belongs to argus-llm;
  nothing in this folder may link llama.
- A caller that cannot reach argus-llm degrades to a documented envelope,
  never a crash.

## Known wart

`llm-service.hxx` carries both the wire DTOs and the in-process engine's
class declaration, because they were never separated. Splitting the DTOs
into their own header (the shape `tts-wire.hxx` has) would let argus-llm
stop linking a module named "client" for its own contract.
