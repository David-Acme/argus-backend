#pragma once

#include <cstdint>
#include <llm/llm-service.hxx>
#include <string>

struct LlmRemoteConfig
{
  std::string url;
  int timeoutMs{120000};

  bool enabled() const { return !url.empty(); }

  static LlmRemoteConfig resolve();
};

struct LlmStreamInput
{
  ChatRequest request;
  TokenCallback onToken;
  LlmPrefillStats* stats{nullptr};
};

class LlmHttpClient
{
public:
  LlmHttpClient(std::string baseUrl, int timeoutMs);

  std::string chat(const ChatRequest& request) const;

  void chatStream(const LlmStreamInput& input) const;

private:
  std::string chatBody(const ChatRequest& request) const;

  std::string baseUrl_;
  int timeoutMs_;
};
