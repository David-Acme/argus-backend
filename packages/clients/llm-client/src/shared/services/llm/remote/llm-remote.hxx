#pragma once

#include <cstdint>
#include <shared/services/llm/llm-service.hxx>
#include <string>

// LLM cutover plumbing: the voice session streams from argus-llm once llm.remote_url is set.
struct LlmRemoteConfig
{
  std::string url;
  int timeoutMs{120000};

  bool enabled() const { return !url.empty(); }

  // Reads llm.remote_url / llm.remote_timeout_ms from the loaded config.
  static LlmRemoteConfig resolve();
};

// One streamed chat call; stats, when non-null, receives the final sentinel's prefill stats.
struct LlmStreamInput
{
  ChatRequest request;
  TokenCallback onToken;
  LlmPrefillStats* stats{nullptr};
};

// HTTP client for the argus-llm internal wire; throws std::runtime_error with the frozen envelope error.
class LlmHttpClient
{
public:
  LlmHttpClient(std::string baseUrl, int timeoutMs);

  // Full completion text out (the non-streaming leg, Connection: close).
  std::string chat(const ChatRequest& request) const;

  // Streams one generation: chunks reach onToken in arrival order; the sentinel triggers onToken("", true).
  void chatStream(const LlmStreamInput& input) const;

private:
  std::string chatBody(const ChatRequest& request) const;

  std::string baseUrl_;
  int timeoutMs_;
};
