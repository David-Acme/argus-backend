#pragma once

#include <cstdint>
#include <shared/services/llm/llm-service.hxx>
#include <string>

// Cutover plumbing for the LLM engine (Ruling BU): the voice session streams
// its replies from argus-llm (:7032) once llm.remote_url is configured; the
// legacy in-process LlmService stays boot-initialized for MemoryService
// (Ruling BF) and is never reached from this path.
struct LlmRemoteConfig
{
  std::string url;
  int timeoutMs{120000};

  bool enabled() const { return !url.empty(); }

  // Reads llm.remote_url / llm.remote_timeout_ms from the loaded config.
  static LlmRemoteConfig resolve();
};

// Parameter struct for one streamed chat call (AGENTS rule 2). stats, when
// non-null, receives the prefill stats carried by the final sentinel line.
struct LlmStreamInput
{
  ChatRequest request;
  TokenCallback onToken;
  LlmPrefillStats* stats{nullptr};
};

// HTTP client for the argus-llm internal wire (Ruling BT): the chat endpoint
// (frozen-envelope JSON out) and the chunked chat-stream endpoint (token
// chunks in wire order, terminated by the JSON sentinel line). Throws
// std::runtime_error carrying the frozen envelope error on failure.
class LlmHttpClient
{
public:
  LlmHttpClient(std::string baseUrl, int timeoutMs);

  // Full completion text out (the non-streaming leg, Connection: close).
  std::string chat(const ChatRequest& request) const;

  // Streams one generation: every HTTP chunk is delivered to onToken as it
  // arrives (arrival order preserved); the sentinel line triggers
  // onToken("", true) and is never surfaced as a token.
  void chatStream(const LlmStreamInput& input) const;

private:
  std::string chatBody(const ChatRequest& request) const;

  std::string baseUrl_;
  int timeoutMs_;
};
