#pragma once

#include <shared/services/llm/llm-service.hxx>
#include <shared/services/llm/remote/llm-remote.hxx>
#include <shared/services/memory/memory-chat.hxx>
#include <string>
#include <utility>

// Extracted-service substrate: worker chats go over the argus-llm wire, never busy.
class WireMemoryChat final : public IMemoryChat
{
public:
  WireMemoryChat(std::string baseUrl, int timeoutMs)
      : client_(std::move(baseUrl), timeoutMs)
  {
  }

  bool available() const override { return true; }
  bool busy() const override { return false; }
  std::string chat(const ChatRequest& request) const override
  {
    return client_.chat(request);
  }

private:
  LlmHttpClient client_;
};
